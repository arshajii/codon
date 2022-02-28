#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "codon/compiler/compiler.h"
#include "codon/compiler/error.h"
#include "codon/compiler/jit.h"
#include "codon/util/common.h"
#include "llvm/Support/CommandLine.h"

namespace {
void versMsg(llvm::raw_ostream &out) {
  out << CODON_VERSION_MAJOR << "." << CODON_VERSION_MINOR << "." << CODON_VERSION_PATCH
      << "\n";
}

const std::vector<std::string> &supportedExtensions() {
  static const std::vector<std::string> extensions = {".codon", ".py"};
  return extensions;
}

bool hasExtension(const std::string &filename, const std::string &extension) {
  return filename.size() >= extension.size() &&
         filename.compare(filename.size() - extension.size(), extension.size(),
                          extension) == 0;
}

std::string trimExtension(const std::string &filename, const std::string &extension) {
  if (hasExtension(filename, extension)) {
    return filename.substr(0, filename.size() - extension.size());
  } else {
    return filename;
  }
}

std::string makeOutputFilename(const std::string &filename,
                               const std::string &extension) {
  for (const auto &ext : supportedExtensions()) {
    if (hasExtension(filename, ext))
      return trimExtension(filename, ext) + extension;
  }
  return filename + extension;
}

void display(const codon::error::ParserErrorInfo &e) {
  for (auto &msg : e) {
    codon::compilationError(msg.getMessage(), msg.getFile(), msg.getLine(),
                            msg.getColumn(),
                            /*terminate=*/false);
  }
}

void initLogFlags(const llvm::cl::opt<std::string> &log) {
  codon::getLogger().parse(log);
  if (auto *d = getenv("CODON_DEBUG"))
    codon::getLogger().parse(std::string(d));
}

enum BuildKind { LLVM, Bitcode, Object, Executable, Detect };
enum OptMode { Debug, Release };
} // namespace

int docMode(const std::vector<const char *> &args, const std::string &argv0) {
  llvm::cl::ParseCommandLineOptions(args.size(), args.data());
  std::vector<std::string> files;
  for (std::string line; std::getline(std::cin, line);)
    files.push_back(line);

  auto compiler = std::make_unique<codon::Compiler>(args[0]);
  bool failed = false;
  auto result = compiler->docgen(files);
  llvm::handleAllErrors(result.takeError(),
                        [&failed](const codon::error::ParserErrorInfo &e) {
                          display(e);
                          failed = true;
                        });
  if (failed)
    return EXIT_FAILURE;

  fmt::print("{}\n", *result);
  return EXIT_SUCCESS;
}

std::unique_ptr<codon::Compiler> processSource(const std::vector<const char *> &args,
                                               bool standalone) {
  llvm::cl::opt<std::string> input(llvm::cl::Positional, llvm::cl::desc("<input file>"),
                                   llvm::cl::init("-"));
  auto regs = llvm::cl::getRegisteredOptions();
  llvm::cl::opt<OptMode> optMode(
      llvm::cl::desc("optimization mode"),
      llvm::cl::values(
          clEnumValN(Debug, regs.find("debug") != regs.end() ? "default" : "debug",
                     "Turn off compiler optimizations and show backtraces"),
          clEnumValN(Release, "release",
                     "Turn on compiler optimizations and disable debug info")),
      llvm::cl::init(Debug));
  llvm::cl::list<std::string> defines(
      "D", llvm::cl::Prefix,
      llvm::cl::desc("Add static variable definitions. The syntax is <name>=<value>"));
  llvm::cl::list<std::string> disabledOpts(
      "disable-opt", llvm::cl::desc("Disable the specified IR optimization"));
  llvm::cl::list<std::string> plugins("plugin",
                                      llvm::cl::desc("Load specified plugin"));
  llvm::cl::opt<std::string> log("log", llvm::cl::desc("Enable given log streams"));

  llvm::cl::ParseCommandLineOptions(args.size(), args.data());
  initLogFlags(log);

  auto &exts = supportedExtensions();
  if (input != "-" && std::find_if(exts.begin(), exts.end(), [&](auto &ext) {
                        return hasExtension(input, ext);
                      }) == exts.end())
    codon::compilationError(
        "input file is expected to be a .codon/.py file, or '-' for stdin");

  std::unordered_map<std::string, std::string> defmap;
  for (const auto &define : defines) {
    auto eq = define.find('=');
    if (eq == std::string::npos || !eq) {
      codon::compilationWarning("ignoring malformed definition: " + define);
      continue;
    }

    auto name = define.substr(0, eq);
    auto value = define.substr(eq + 1);

    if (defmap.find(name) != defmap.end()) {
      codon::compilationWarning("ignoring duplicate definition: " + define);
      continue;
    }

    defmap.emplace(name, value);
  }

  const bool isDebug = (optMode == OptMode::Debug);
  std::vector<std::string> disabledOptsVec(disabledOpts);
  auto compiler = std::make_unique<codon::Compiler>(args[0], isDebug, disabledOptsVec);
  compiler->getLLVMVisitor()->setStandalone(standalone);

  // load plugins
  for (const auto &plugin : plugins) {
    bool failed = false;
    llvm::handleAllErrors(
        compiler->load(plugin), [&failed](const codon::error::PluginErrorInfo &e) {
          codon::compilationError(e.getMessage(), /*file=*/"", /*line=*/0, /*col=*/0,
                                  /*terminate=*/false);
          failed = true;
        });
    if (failed)
      return {};
  }

  bool failed = false;
  llvm::handleAllErrors(compiler->parseFile(input, /*isTest=*/0, defmap),
                        [&failed](const codon::error::ParserErrorInfo &e) {
                          display(e);
                          failed = true;
                        });
  if (failed)
    return {};

  {
    TIME("compile");
    llvm::cantFail(compiler->compile());
  }
  return compiler;
}

int runMode(const std::vector<const char *> &args) {
  llvm::cl::list<std::string> libs(
      "l", llvm::cl::desc("Load and link the specified library"));
  llvm::cl::list<std::string> seqArgs(llvm::cl::ConsumeAfter,
                                      llvm::cl::desc("<program arguments>..."));
  auto compiler = processSource(args, /*standalone=*/false);
  if (!compiler)
    return EXIT_FAILURE;
  std::vector<std::string> libsVec(libs);
  std::vector<std::string> argsVec(seqArgs);
  argsVec.insert(argsVec.begin(), compiler->getInput());
  compiler->getLLVMVisitor()->run(argsVec, libsVec);
  return EXIT_SUCCESS;
}

namespace {
std::string jitExec(codon::jit::JIT *jit, const std::string &code) {
  auto result = jit->exec(code);
  if (auto err = result.takeError()) {
    std::string output;
    llvm::handleAllErrors(
        std::move(err), [](const codon::error::ParserErrorInfo &e) { display(e); },
        [&output](const codon::error::RuntimeErrorInfo &e) {
          std::stringstream buf;
          buf << e.getOutput();
          buf << "\n\033[1mBacktrace:\033[0m\n";
          for (const auto &line : e.getBacktrace()) {
            buf << "  " << line << "\n";
          }
          output = buf.str();
        });
    return output;
  }
  return *result;
}
} // namespace

int jitMode(const std::vector<const char *> &args) {
  llvm::cl::list<std::string> plugins("plugin",
                                      llvm::cl::desc("Load specified plugin"));
  llvm::cl::opt<std::string> log("log", llvm::cl::desc("Enable given log streams"));
  llvm::cl::ParseCommandLineOptions(args.size(), args.data());
  initLogFlags(log);
  codon::jit::JIT jit(args[0]);

  // load plugins
  for (const auto &plugin : plugins) {
    bool failed = false;
    llvm::handleAllErrors(jit.getCompiler()->load(plugin),
                          [&failed](const codon::error::PluginErrorInfo &e) {
                            codon::compilationError(e.getMessage(), /*file=*/"",
                                                    /*line=*/0, /*col=*/0,
                                                    /*terminate=*/false);
                            failed = true;
                          });
    if (failed)
      return EXIT_FAILURE;
  }

  llvm::cantFail(jit.init());
  fmt::print(">>> Codon JIT v{} <<<\n", CODON_VERSION);
  std::string code;
  for (std::string line; std::getline(std::cin, line);) {
    if (line != "#%%") {
      code += line + "\n";
    } else {
      fmt::print("{}[done]\n", jitExec(&jit, code));
      code = "";
      fflush(stdout);
    }
  }
  if (!code.empty())
    fmt::print("{}[done]\n", jitExec(&jit, code));
  return EXIT_SUCCESS;
}

int buildMode(const std::vector<const char *> &args) {
  llvm::cl::list<std::string> libs(
      "l", llvm::cl::desc("Link the specified library (only for executables)"));
  llvm::cl::opt<BuildKind> buildKind(
      llvm::cl::desc("output type"),
      llvm::cl::values(clEnumValN(LLVM, "llvm", "Generate LLVM IR"),
                       clEnumValN(Bitcode, "bc", "Generate LLVM bitcode"),
                       clEnumValN(Object, "obj", "Generate native object file"),
                       clEnumValN(Executable, "exe", "Generate executable"),
                       clEnumValN(Detect, "detect",
                                  "Detect output type based on output file extension")),
      llvm::cl::init(Detect));
  llvm::cl::opt<std::string> output(
      "o",
      llvm::cl::desc(
          "Write compiled output to specified file. Supported extensions: "
          "none (executable), .o (object file), .ll (LLVM IR), .bc (LLVM bitcode)"));

  auto compiler = processSource(args, /*standalone=*/true);
  if (!compiler)
    return EXIT_FAILURE;
  std::vector<std::string> libsVec(libs);

  if (output.empty() && compiler->getInput() == "-")
    codon::compilationError("output file must be specified when reading from stdin");
  std::string extension;
  switch (buildKind) {
  case BuildKind::LLVM:
    extension = ".ll";
    break;
  case BuildKind::Bitcode:
    extension = ".bc";
    break;
  case BuildKind::Object:
    extension = ".o";
    break;
  case BuildKind::Executable:
  case BuildKind::Detect:
    extension = "";
    break;
  default:
    seqassert(0, "unknown build kind");
  }
  const std::string filename =
      output.empty() ? makeOutputFilename(compiler->getInput(), extension) : output;
  switch (buildKind) {
  case BuildKind::LLVM:
    compiler->getLLVMVisitor()->writeToLLFile(filename);
    break;
  case BuildKind::Bitcode:
    compiler->getLLVMVisitor()->writeToBitcodeFile(filename);
    break;
  case BuildKind::Object:
    compiler->getLLVMVisitor()->writeToObjectFile(filename);
    break;
  case BuildKind::Executable:
    compiler->getLLVMVisitor()->writeToExecutable(filename, libsVec);
    break;
  case BuildKind::Detect:
    compiler->getLLVMVisitor()->compile(filename, libsVec);
    break;
  default:
    seqassert(0, "unknown build kind");
  }

  return EXIT_SUCCESS;
}

#ifdef CODON_JUPYTER
namespace codon {
int startJupyterKernel(const std::string &argv0,
                       const std::vector<std::string> &plugins,
                       const std::string &configPath);
}
#endif
int jupyterMode(const std::vector<const char *> &args) {
#ifdef CODON_JUPYTER
  llvm::cl::list<std::string> plugins("plugin",
                                      llvm::cl::desc("Load specified plugin"));
  llvm::cl::opt<std::string> input(llvm::cl::Positional,
                                   llvm::cl::desc("<connection file>"),
                                   llvm::cl::init("connection.json"));
  llvm::cl::ParseCommandLineOptions(args.size(), args.data());
  int code = codon::startJupyterKernel(args[0], plugins, input);
  return code;
#else
  fmt::print("Jupyter support not included. Please recompile with "
             "-DCODON_JUPYTER.");
  return EXIT_FAILURE;
#endif
}

void showCommandsAndExit() {
  codon::compilationError("Available commands: seqc <run|build|doc>");
}

int otherMode(const std::vector<const char *> &args) {
  llvm::cl::opt<std::string> input(llvm::cl::Positional, llvm::cl::desc("<mode>"));
  llvm::cl::extrahelp("\nMODES:\n\n"
                      "  run   - run a program interactively\n"
                      "  build - build a program\n"
                      "  doc   - generate program documentation\n");
  llvm::cl::ParseCommandLineOptions(args.size(), args.data());

  if (!input.empty())
    showCommandsAndExit();
  return EXIT_SUCCESS;
}

int main(int argc, const char **argv) {
  if (argc < 2)
    showCommandsAndExit();

  llvm::cl::SetVersionPrinter(versMsg);
  std::vector<const char *> args{argv[0]};
  for (int i = 2; i < argc; i++)
    args.push_back(argv[i]);

  std::string mode(argv[1]);
  std::string argv0 = std::string(args[0]) + " " + mode;
  if (mode == "run") {
    args[0] = argv0.data();
    return runMode(args);
  }
  if (mode == "build") {
    args[0] = argv0.data();
    return buildMode(args);
  }
  if (mode == "doc") {
    const char *oldArgv0 = args[0];
    args[0] = argv0.data();
    return docMode(args, oldArgv0);
  }
  if (mode == "jit") {
    args[0] = argv0.data();
    return jitMode(args);
  }
  if (mode == "jupyter") {
    args[0] = argv0.data();
    return jupyterMode(args);
  }
  return otherMode({argv, argv + argc});
}