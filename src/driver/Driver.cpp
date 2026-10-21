#include "driver/Driver.h"

#include "codegen/CodeGen.h"
#include "driver/Linker.h"
#include "parse/Parser.h"
#include "sema/Sema.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tz {
namespace {

const char kUsage[] = R"(usage: tz <command> [options] <file.ts> [arguments]

commands:
  build     compile a program into a native executable
  run       compile a program and run it; arguments after the file go to it
  version   print the version

options:
  -o <path>         where to write the output (build only)
  -O0 -O1 -O2 -O3   optimization level (default: -O2)
  --emit-llvm       write LLVM IR (.ll) instead of an executable (build only)
)";

// Exit codes of tz itself. `tz run` otherwise exits with the program's code.
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

enum class Command { Build, Run };

struct Options {
  Command command = Command::Build;
  std::string input;
  std::string output;
  unsigned optLevel = 2;
  bool emitLLVM = false;
  std::vector<std::string> programArgs;
};

int usageError(const llvm::Twine &message) {
  llvm::errs() << "tz: " << message << "\n\n" << kUsage;
  return kExitUsage;
}

// Returns -1 if the options are complete, else the code tz should exit with.
int parseArgs(llvm::ArrayRef<const char *> args, Options &opts) {
  if (args.empty()) {
    llvm::errs() << kUsage;
    return kExitUsage;
  }

  llvm::StringRef command = args[0];
  if (command == "help" || command == "-h" || command == "--help") {
    llvm::outs() << kUsage;
    return 0;
  }
  if (command == "version" || command == "--version") {
    llvm::outs() << "tz version " TZ_VERSION " (LLVM " LLVM_VERSION_STRING ")\n";
    return 0;
  }
  if (command == "build")
    opts.command = Command::Build;
  else if (command == "run")
    opts.command = Command::Run;
  else
    return usageError("unknown command '" + command + "'");

  for (size_t i = 1; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (!opts.input.empty()) {
      // Everything after the source file belongs to the program.
      opts.programArgs.push_back(arg.str());
    } else if (arg == "-o") {
      if (++i == args.size())
        return usageError("'-o' needs a path");
      opts.output = args[i];
    } else if (arg.size() == 3 && arg.starts_with("-O") && arg[2] >= '0' && arg[2] <= '3') {
      opts.optLevel = arg[2] - '0';
    } else if (arg == "--emit-llvm") {
      opts.emitLLVM = true;
    } else if (arg.starts_with("-")) {
      return usageError("unknown option '" + arg + "'");
    } else {
      opts.input = arg.str();
    }
  }

  if (opts.input.empty())
    return usageError("no source file given");
  if (opts.command == Command::Build && !opts.programArgs.empty())
    return usageError("unexpected argument '" + opts.programArgs.front() + "' after the source file");
  if (opts.command == Command::Run && (opts.emitLLVM || !opts.output.empty()))
    return usageError("'-o' and '--emit-llvm' only apply to 'tz build'");
  return -1;
}

void initializeTargets() {
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmPrinter();
  LLVMInitializeAArch64TargetInfo();
  LLVMInitializeAArch64Target();
  LLVMInitializeAArch64TargetMC();
  LLVMInitializeAArch64AsmPrinter();
}

// Front end: source text to verified LLVM IR. Returns null after reporting
// errors.
std::unique_ptr<llvm::Module> compileToIR(llvm::StringRef path, llvm::LLVMContext &ctx) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    llvm::errs() << "tz: cannot read '" << path << "': " << buffer.getError().message() << "\n";
    return nullptr;
  }
  llvm::SourceMgr sourceMgr;
  unsigned bufferId = sourceMgr.AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  Diagnostics diags(sourceMgr);

  AstContext ast;
  Lexer lexer(sourceMgr.getMemoryBuffer(bufferId)->getBuffer(), diags);
  SourceFile file = Parser(lexer, ast, diags).parseSourceFile();
  if (diags.hasErrors())
    return nullptr;

  Sema(ast, diags).check(file);
  if (diags.hasErrors())
    return nullptr;

  std::unique_ptr<llvm::Module> module = CodeGen(ctx, llvm::sys::path::filename(path)).generate(file);
  if (llvm::verifyModule(*module, &llvm::errs())) {
    llvm::errs() << "tz: internal error: the generated LLVM IR is invalid\n";
    return nullptr;
  }
  return module;
}

std::unique_ptr<llvm::TargetMachine> createTargetMachine(const llvm::Triple &triple, unsigned optLevel) {
  std::string error;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) {
    llvm::errs() << "tz: " << error << "\n";
    return nullptr;
  }
  static const llvm::CodeGenOptLevel levels[] = {
      llvm::CodeGenOptLevel::None, llvm::CodeGenOptLevel::Less,
      llvm::CodeGenOptLevel::Default, llvm::CodeGenOptLevel::Aggressive};
  llvm::TargetOptions options;
  return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      triple, "generic", "", options, std::nullopt, std::nullopt, levels[optLevel]));
}

void optimize(llvm::Module &module, llvm::TargetMachine &tm, unsigned optLevel) {
  // The analysis managers must be destroyed in this reverse order.
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

  llvm::PassBuilder pb(&tm);
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  static const llvm::OptimizationLevel levels[] = {
      llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1, llvm::OptimizationLevel::O2,
      llvm::OptimizationLevel::O3};
  llvm::ModulePassManager mpm = optLevel == 0
                                    ? pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0)
                                    : pb.buildPerModuleDefaultPipeline(levels[optLevel]);
  mpm.run(module, mam);
}

bool writeFile(llvm::StringRef path, llvm::function_ref<void(llvm::raw_pwrite_stream &)> write) {
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    llvm::errs() << "tz: cannot write '" << path << "': " << ec.message() << "\n";
    return false;
  }
  write(out);
  out.close();
  if (out.has_error()) {
    llvm::errs() << "tz: cannot write '" << path << "': " << out.error().message() << "\n";
    out.clear_error();
    return false;
  }
  return true;
}

bool emitObjectFile(llvm::Module &module, llvm::TargetMachine &tm, llvm::StringRef path) {
  bool supported = true;
  bool written = writeFile(path, [&](llvm::raw_pwrite_stream &out) {
    llvm::legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
      supported = false;
      return;
    }
    pm.run(module);
  });
  if (!supported)
    llvm::errs() << "tz: the target cannot emit object files\n";
  return written && supported;
}

// The runtime library sits next to the tz executable.
std::string runtimeLibraryPath(const char *argv0, const llvm::Triple &triple) {
  std::string self = llvm::sys::fs::getMainExecutable(argv0, (void *)(intptr_t)&runDriver);
  llvm::SmallString<256> path(llvm::sys::path::parent_path(self));
  llvm::sys::path::append(path, triple.isOSWindows() ? "tzrt.lib" : "libtzrt.a");
  return std::string(path);
}

int build(const Options &opts, llvm::StringRef output, const char *argv0) {
  llvm::LLVMContext ctx;
  std::unique_ptr<llvm::Module> module = compileToIR(opts.input, ctx);
  if (!module)
    return kExitError;

  llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  std::unique_ptr<llvm::TargetMachine> tm = createTargetMachine(triple, opts.optLevel);
  if (!tm)
    return kExitError;
  module->setTargetTriple(triple);
  module->setDataLayout(tm->createDataLayout());
  optimize(*module, *tm, opts.optLevel);

  if (opts.emitLLVM)
    return writeFile(output, [&](llvm::raw_pwrite_stream &out) { module->print(out, nullptr); })
               ? 0
               : kExitError;

  std::string runtime = runtimeLibraryPath(argv0, triple);
  if (!llvm::sys::fs::exists(runtime)) {
    llvm::errs() << "tz: the runtime library is missing: " << runtime << "\n";
    return kExitError;
  }

  llvm::SmallString<128> object;
  if (std::error_code ec = llvm::sys::fs::createTemporaryFile("tz", "obj", object)) {
    llvm::errs() << "tz: cannot create a temporary file: " << ec.message() << "\n";
    return kExitError;
  }
  llvm::FileRemover removeObject(object);
  if (!emitObjectFile(*module, *tm, object))
    return kExitError;

  std::string errors;
  if (!linkExecutable(triple, {std::string(object), runtime}, output, errors)) {
    llvm::errs() << errors << "tz: linking failed\n";
    return kExitError;
  }
  if (!errors.empty())
    llvm::errs() << errors; // linker warnings
  return 0;
}

int run(const Options &opts, const char *argv0) {
  llvm::SmallString<128> exe;
  if (std::error_code ec = llvm::sys::fs::createTemporaryFile("tz-run", "exe", exe)) {
    llvm::errs() << "tz: cannot create a temporary file: " << ec.message() << "\n";
    return kExitError;
  }
  llvm::FileRemover removeExe(exe);
  if (int status = build(opts, exe, argv0))
    return status;

  std::vector<llvm::StringRef> args{exe};
  for (const std::string &arg : opts.programArgs)
    args.push_back(arg);
  std::string error;
  int code = llvm::sys::ExecuteAndWait(exe, args, std::nullopt, {}, 0, 0, &error);
  if (code == -1) {
    llvm::errs() << "tz: cannot run the program: " << error << "\n";
    return kExitError;
  }
  if (code == -2) {
    llvm::errs() << "tz: the program crashed: " << error << "\n";
    return 2;
  }
  return code;
}

std::string defaultOutput(const Options &opts) {
  llvm::SmallString<128> name(llvm::sys::path::stem(opts.input));
  if (opts.emitLLVM)
    name += ".ll";
  else if (llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSWindows())
    name += ".exe";
  return std::string(name);
}

} // namespace

int runDriver(int argc, char **argv) {
  Options opts;
  std::vector<const char *> args(argv + 1, argv + argc);
  if (int code = parseArgs(args, opts); code >= 0)
    return code;

  initializeTargets();
  if (opts.command == Command::Run)
    return run(opts, argv[0]);
  return build(opts, opts.output.empty() ? defaultOutput(opts) : opts.output, argv[0]);
}

} // namespace tz
