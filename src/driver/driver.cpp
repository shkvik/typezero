#include "driver/driver.h"

#include "codegen/codegen.h"
#include "driver/licenses.h"
#include "driver/linker.h"
#include "driver/loader.h"
#include "pkg/commands.h"
#include "pkg/project.h"
#include "sema/overflow.h"
#include "sema/parallel.h"
#include "sema/sema.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tz {
namespace {

const char kUsage[] = R"(usage: tz <command> [options] [<file.ts>] [arguments]

commands:
  build     compile a program into a native executable
  run       compile a program and run it; arguments after the file go to it
  init      start a project: tz.json, main.ts and .gitignore
  add       add packages to tz.json: tz add zod, tz add jsr:@std/path@^1,
            tz add github:user/repo@v1.2.0, tz add ../lib
  remove    remove packages from tz.json
  update    move packages to the newest versions tz.json allows
  port      copy a TypeScript library and adapt it to tz
  licenses  print the licenses of the code of others an executable embeds,
            which go along with it: tz licenses app.exe > LICENSES.txt
  version   print the version

Without a file, build and run take "main" of tz.json.

options:
  -o <path>         where to write the output (build only)
  --emit-llvm       write LLVM IR (.ll) instead of an executable (build only)
  --explain-parallel
                    print what runs in parallel and why, instead of building
                    (build only)
)";
// Not in the usage: -O0 turns optimization off, runtime inlining included.
// The tests run every program both ways, and with --emit-llvm it shows the IR
// as CodeGen emits it, except that generators are split into the functions
// of their coroutines (LLVM has to do that at any level).

// Exit codes of tz itself. `tz run` otherwise exits with the program's code.
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

enum class Command { Build, Run, Licenses };

struct Options {
  Command command = Command::Build;
  std::string input;
  std::string output;
  // The name of the executable when it is not named after the input: the
  // project's, for a build of its "main".
  std::string outputStem;
  bool optimize = true;
  bool emitLLVM = false;
  bool explainParallel = false;
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
  else if (command == "licenses")
    opts.command = Command::Licenses;
  else
    return usageError("unknown command '" + command + "'");

  bool programArgs = false;
  for (size_t i = 1; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (!opts.input.empty() || programArgs) {
      // Everything after the source file belongs to the program.
      opts.programArgs.push_back(arg.str());
    } else if (arg == "--" && opts.command == Command::Run) {
      // `tz run -- a b`: the program is the project's, and the rest is its.
      programArgs = true;
    } else if (arg == "-o") {
      if (++i == args.size())
        return usageError("'-o' needs a path");
      opts.output = args[i];
    } else if (arg == "-O0") {
      opts.optimize = false;
    } else if (arg == "--emit-llvm") {
      opts.emitLLVM = true;
    } else if (arg == "--explain-parallel") {
      opts.explainParallel = true;
    } else if (arg.starts_with("-")) {
      return usageError("unknown option '" + arg + "'");
    } else if (opts.command == Command::Run && !arg.ends_with_insensitive(".ts")) {
      // Not a source file: the project's program runs, and this is its
      // first argument.
      opts.programArgs.push_back(arg.str());
      programArgs = true;
    } else {
      opts.input = arg.str();
    }
  }

  if (opts.command == Command::Licenses) {
    if (opts.input.empty())
      return usageError("no executable given");
    if (!opts.programArgs.empty() || opts.emitLLVM || !opts.output.empty())
      return usageError("'tz licenses' takes one executable");
  }
  if (opts.command == Command::Build && !opts.programArgs.empty())
    return usageError("unexpected argument '" + opts.programArgs.front() + "' after the source file");
  if (opts.command == Command::Run && (opts.emitLLVM || !opts.output.empty()))
    return usageError("'-o' and '--emit-llvm' only apply to 'tz build'");
  if (opts.command == Command::Run && opts.explainParallel)
    return usageError("'--explain-parallel' only applies to 'tz build'");
  if (opts.explainParallel && (opts.emitLLVM || !opts.output.empty()))
    return usageError("'--explain-parallel' builds nothing, so it takes no '-o' or '--emit-llvm'");
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

// Without a file, `tz build` and `tz run` build the "main" of the project
// around the current directory. Returns -1 when the input is known, else the
// code tz should exit with.
int findMain(Options &opts) {
  if (!opts.input.empty())
    return -1;
  std::string manifestPath = pkg::findManifest("");
  if (manifestPath.empty())
    return usageError(opts.programArgs.empty()
                          ? "no source file given, and there is no tz.json here or above"
                          : "'" + opts.programArgs.front() +
                                "' is not a .ts file, and there is no tz.json here or above "
                                "whose \"main\" would run");
  pkg::Manifest manifest;
  if (!pkg::readManifest(manifestPath, manifest))
    return kExitError;
  if (manifest.main.empty()) {
    llvm::errs() << "tz: " << manifestPath
                 << " has no \"main\": add \"main\": \"main.ts\", or name the file to build\n";
    return kExitError;
  }
  llvm::SmallString<256> main(manifest.dir);
  llvm::sys::path::append(main, manifest.main);
  opts.input = std::string(main);
  // "@scope/api" builds api.exe.
  llvm::StringRef name = manifest.name;
  name = name.substr(name.rfind('/') + 1);
  if (!name.empty())
    opts.outputStem = name.str();
  return -1;
}

// The packages of the project the file belongs to: tz.json in its directory
// or above. False after reporting errors; `graph` stays null without a
// project.
bool preparePackages(llvm::StringRef input, pkg::Graph &storage, const pkg::Graph *&graph) {
  graph = nullptr;
  llvm::SmallString<256> dir(input);
  llvm::sys::fs::make_absolute(dir);
  llvm::sys::path::remove_filename(dir);
  std::string manifestPath = pkg::findManifest(dir);
  if (manifestPath.empty())
    return true;
  pkg::Manifest manifest;
  if (!pkg::readManifest(manifestPath, manifest) ||
      !pkg::preparePackages(manifest, pkg::PrepareOptions(), storage))
    return false;
  graph = &storage;
  return true;
}

// The program checked: the entry file and the files it imports, loaded and
// checked by Sema. Returns false after reporting errors.
struct CheckedProgram {
  llvm::SourceMgr sourceMgr;
  std::vector<std::unique_ptr<AstContext>> asts;
  std::unique_ptr<Diagnostics> diags;
  SourceFile file;
};

bool checkProgram(llvm::StringRef path, CheckedProgram &program) {
  pkg::Graph packages;
  const pkg::Graph *graph;
  if (!preparePackages(path, packages, graph))
    return false;
  llvm::SourceMgr &sourceMgr = program.sourceMgr;
  // A program whose emitters get their event maps from its emit() calls is
  // checked in rounds (see EventInference): each round reads it anew, and
  // only the last one says what is wrong and is compiled.
  constexpr unsigned kInferenceRounds = 8;
  EventInference inference;
  std::vector<std::unique_ptr<AstContext>> &asts = program.asts;
  std::unique_ptr<Diagnostics> &diags = program.diags;
  SourceFile &file = program.file;
  for (unsigned round = 0;; ++round) {
    diags = std::make_unique<Diagnostics>(sourceMgr);
    diags->setRecording(true);
    AstContext &ast = *asts.emplace_back(std::make_unique<AstContext>());
    file = SourceFile();
    if (!loadProgram(path, sourceMgr, ast, *diags, file, graph) || diags->hasErrors()) {
      diags->replay();
      return false;
    }
    Sema sema(ast, *diags);
    sema.setEventInference(&inference);
    sema.check(file);
    if (!inference.open || inference.final)
      break;
    // Another round, with what this one found; the last one once a round
    // finds nothing more.
    if (!inference.changed || round + 2 >= kInferenceRounds)
      inference.final = true;
    inference.changed = inference.open = false;
  }
  // Warnings where an int may overflow (docs/small.md, section 6), of the
  // last round only, also next to errors; they do not stop the build.
  checkIntOverflow(file, *diags);
  diags->replay();
  return !diags->hasErrors();
}

// Front end: the entry file and the files it imports to verified LLVM IR.
// Returns null after reporting errors.
std::unique_ptr<llvm::Module> compileToIR(llvm::StringRef path, llvm::LLVMContext &ctx,
                                          const llvm::DataLayout &layout) {
  CheckedProgram program;
  if (!checkProgram(path, program))
    return nullptr;
  CodeGen codegen(ctx, llvm::sys::path::filename(path), layout);
  codegen.setSourceMgr(&program.sourceMgr);
  std::unique_ptr<llvm::Module> module = codegen.generate(program.file);
  if (llvm::verifyModule(*module, &llvm::errs())) {
    llvm::errs() << "tz: internal error: the generated LLVM IR is invalid\n";
    return nullptr;
  }
  return module;
}

// `tz build --explain-parallel`: the program checked, then what of it may
// run in parallel and why (docs/async.md, section 5), on stdout. Nothing is
// built.
int explain(llvm::StringRef path) {
  // Checked as a build checks it: in the rounds that find the event maps.
  CheckedProgram program;
  if (!checkProgram(path, program))
    return kExitError;
  explainParallel(program.file, program.sourceMgr, llvm::outs());
  return 0;
}

std::unique_ptr<llvm::TargetMachine> createTargetMachine(const llvm::Triple &triple, bool optimize) {
  std::string error;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) {
    llvm::errs() << "tz: " << error << "\n";
    return nullptr;
  }
  llvm::TargetOptions options;
  return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      triple, "generic", "", options, std::nullopt, std::nullopt,
      optimize ? llvm::CodeGenOptLevel::Default : llvm::CodeGenOptLevel::None));
}

// The runtime files sit next to the tz executable.
std::string runtimeFile(const char *argv0, llvm::StringRef name) {
  std::string self = llvm::sys::fs::getMainExecutable(argv0, (void *)(intptr_t)&runDriver);
  llvm::SmallString<256> path(llvm::sys::path::parent_path(self));
  llvm::sys::path::append(path, name);
  return std::string(path);
}

// A hot runtime function with more instructions than this, counting the
// static helpers it brings along, is not copied into programs after all: the
// inliner would rarely take it, and optimizing it again for every program
// only costs compile time.
constexpr unsigned kRuntimeCopyLimit = 100;

// The instructions of `fn` and of the static functions it refers to, which
// would be copied with it.
unsigned copySize(llvm::Function &fn, llvm::SmallPtrSetImpl<llvm::Function *> &seen) {
  unsigned size = fn.getInstructionCount();
  for (llvm::Instruction &inst : llvm::instructions(fn))
    for (llvm::Value *operand : inst.operands()) {
      auto *callee = llvm::dyn_cast<llvm::Function>(operand);
      if (callee && callee->hasLocalLinkage() && !callee->isDeclaration() &&
          seen.insert(callee).second)
        size += copySize(*callee, seen);
    }
  return size;
}

// Copies the hot runtime functions (TZ_HOT in runtime/tzrt.h) the program
// calls, and the hot ones they call, from the runtime bitcode into the
// program's module as available_externally: the optimizer may inline them
// into the program's loops, and then drops them, so every call that is left
// goes to tzrt.lib, which is linked as usual. Returns false after reporting
// an error.
bool linkRuntime(llvm::Module &module, const std::string &path) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    llvm::errs() << "tz: the runtime is missing: " << path << "\n";
    return false;
  }
  // Lazily: only the functions that end up linked are read in full.
  llvm::Expected<std::unique_ptr<llvm::Module>> runtime =
      llvm::getOwningLazyBitcodeModule(std::move(*buffer), module.getContext());
  if (!runtime) {
    llvm::errs() << "tz: cannot read the runtime " << path << ": "
                 << llvm::toString(runtime.takeError()) << "\n";
    return false;
  }
  (*runtime)->setTargetTriple(module.getTargetTriple());
  (*runtime)->setDataLayout(module.getDataLayout());

  // A call through a declaration that disagrees with the definition would
  // not be inlined, or would pass its arguments wrongly.
  for (llvm::Function &decl : module) {
    llvm::Function *def = decl.isDeclaration() ? (*runtime)->getFunction(decl.getName()) : nullptr;
    if (def && def->getFunctionType() != decl.getFunctionType()) {
      llvm::errs() << "tz: internal error: " << decl.getName()
                   << " is declared differently in CodeGen and in the runtime\n";
      return false;
    }
  }
  // What the copies refer to by name is the one copy in tzrt.lib. State that
  // is static would be duplicated instead: every copied function would get a
  // variable of its own. (Names starting with __ belong to the C library's
  // inline functions.)
  llvm::StringSet<> shared;
  for (llvm::GlobalValue &value : (*runtime)->global_values()) {
    if (value.isDeclaration())
      continue;
    auto *var = llvm::dyn_cast<llvm::GlobalVariable>(&value);
    if (var && var->hasLocalLinkage() && !var->isConstant() && !var->getName().starts_with("__")) {
      llvm::errs() << "tz: internal error: the runtime variable " << var->getName()
                   << " is static\n";
      return false;
    }
    if (value.hasExternalLinkage())
      shared.insert(value.getName());
  }

  // CodeGen declares every runtime function; only those the program calls
  // are to be linked.
  for (llvm::Function &fn : llvm::make_early_inc_range(module))
    if (fn.isDeclaration() && fn.use_empty())
      fn.eraseFromParent();
  if (llvm::Linker::linkModules(module, std::move(*runtime), llvm::Linker::LinkOnlyNeeded)) {
    llvm::errs() << "tz: internal error: cannot link the runtime\n";
    return false;
  }

  for (llvm::Function &fn : module) {
    // clang named a CPU for the runtime; the inliner does not mix functions
    // built for different CPUs, so everything uses the target's default.
    fn.removeFnAttr("target-cpu");
    fn.removeFnAttr("target-features");
    fn.removeFnAttr("tune-cpu");
    if (fn.isDeclaration() || !shared.contains(fn.getName()))
      continue;
    fn.setComdat(nullptr);
    llvm::SmallPtrSet<llvm::Function *, 8> helpers;
    if (!fn.hasFnAttribute(llvm::Attribute::Hot) || copySize(fn, helpers) > kRuntimeCopyLimit)
      fn.deleteBody();
    else
      fn.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
  }
  for (llvm::GlobalVariable &var : module.globals()) {
    if (var.isDeclaration() || !shared.contains(var.getName()))
      continue;
    var.setComdat(nullptr);
    if (var.isConstant()) {
      var.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
    } else {
      var.setInitializer(nullptr);
      var.setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
  }
  // Static helpers that only the dropped bodies called are left over; runPasses
  // removes them before optimizing.
  return true;
}

void runPasses(llvm::Module &module, llvm::TargetMachine &tm, bool optimize) {
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

  llvm::ModulePassManager mpm;
  if (optimize) {
    // First drop what linkRuntime left unused, so it is not optimized for
    // nothing.
    mpm.addPass(llvm::GlobalDCEPass());
    mpm.addPass(pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2));
  } else {
    mpm.addPass(pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0));
  }
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

// Whether `path` is a file that nothing forbids writing but that cannot be
// opened for it: in Windows, an executable that is running.
bool inUse(llvm::StringRef path) {
  namespace fs = llvm::sys::fs;
  if (!fs::is_regular_file(path) || fs::access(path, fs::AccessMode::Write))
    return false;
  int fd;
  std::error_code ec = fs::openFileForWrite(path, fd, fs::CD_OpenExisting, fs::OF_Append);
  if (!ec) {
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);
    return false;
  }
  return ec == std::errc::permission_denied;
}

int build(const Options &opts, llvm::StringRef output, const char *argv0) {
  // The target comes first: code generation needs its data layout to know
  // the size of values.
  llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  std::unique_ptr<llvm::TargetMachine> tm = createTargetMachine(triple, opts.optimize);
  if (!tm)
    return kExitError;

  llvm::LLVMContext ctx;
  std::unique_ptr<llvm::Module> module = compileToIR(opts.input, ctx, tm->createDataLayout());
  if (!module)
    return kExitError;
  module->setTargetTriple(triple);
  if (opts.optimize && !linkRuntime(*module, runtimeFile(argv0, "tzrt.bc")))
    return kExitError;
  runPasses(*module, *tm, opts.optimize);

  if (opts.emitLLVM)
    return writeFile(output, [&](llvm::raw_pwrite_stream &out) { module->print(out, nullptr); })
               ? 0
               : kExitError;
  CryptoUse crypto = cryptoUse(*module);
  addLicenseManifest(*module, {crypto.any, usesEncodingIndexes(*module)});

  std::string runtime = runtimeFile(argv0, triple.isOSWindows() ? "tzrt.lib" : "libtzrt.a");
  if (!llvm::sys::fs::exists(runtime)) {
    llvm::errs() << "tz: the runtime is missing: " << runtime << "\n";
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

  // node:crypto: the glue, and OpenSSL, all of it or only the objects its direct layer calls
  // (docs/crypto.md, section 3).
  std::vector<std::string> inputs = {std::string(object), runtime};
  if (crypto.any) {
    const char *openssl = crypto.full ? "libcrypto.lib" : "libcrypto_direct.lib";
    for (const char *lib : {"tzcrypto.lib", openssl}) {
      std::string path = runtimeFile(argv0, lib);
      if (!llvm::sys::fs::exists(path)) {
        llvm::errs() << "tz: the library of node:crypto is missing: " << path << "\n";
        return kExitError;
      }
      inputs.push_back(path);
    }
    if (crypto.full)
      for (const char *lib : {"ws2_32", "advapi32", "crypt32", "user32"})
        inputs.push_back(std::string("/defaultlib:") + lib);
  }

  std::string errors;
  if (!linkExecutable(triple, inputs, output, errors)) {
    if (inUse(output))
      llvm::errs() << "tz: cannot write '" << output
                   << "': it is in use, probably still running; stop it and build again\n";
    else
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
  llvm::SmallString<128> name(opts.outputStem.empty() ? llvm::sys::path::stem(opts.input)
                                                      : llvm::StringRef(opts.outputStem));
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
  if (!args.empty()) {
    llvm::StringRef command = args[0];
    llvm::ArrayRef<const char *> rest = llvm::ArrayRef(args).drop_front();
    if (command == "init")
      return pkg::commandInit(rest);
    if (command == "add")
      return pkg::commandAdd(rest);
    if (command == "remove")
      return pkg::commandRemove(rest);
    if (command == "update")
      return pkg::commandUpdate(rest);
    if (command == "port")
      return pkg::commandPort(rest);
  }
  if (int code = parseArgs(args, opts); code >= 0)
    return code;
  if (int code = findMain(opts); code >= 0)
    return code;

  if (opts.command == Command::Licenses)
    return printLicenses(opts.input, runtimeFile(argv[0], "licenses"));
  if (opts.explainParallel)
    return explain(opts.input);
  initializeTargets();
  if (opts.command == Command::Run)
    return run(opts, argv[0]);
  return build(opts, opts.output.empty() ? defaultOutput(opts) : opts.output, argv[0]);
}

} // namespace tz
