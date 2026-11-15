#include "compiler/native/native_sources/native_sources.h"
#include "compiler/optimize/target/target.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>
#include <iterator>

namespace tz {

NativeSources::NativeSources(const Target &target, const BuildOptions &options)
    : target(target),
      options(options) {}

NativeSources::ResultEntryBuilt NativeSources::buildEntry(const pkg::NativeEntry &entry,
                                                          llvm::LLVMContext &context) const {
  auto files = this->findFiles(entry);
  if (files.isErr()) {
    return err(files.error());
  }
  auto joined = std::make_unique<llvm::Module>(entry.name, context);
  for (const std::string &file : files.value()) {
    auto built = this->buildFile(entry, file, context);
    if (built.isErr()) {
      return err(built.error());
    }
    auto joinedFile = this->joinFile(entry, *joined, std::move(built.value()));
    if (joinedFile.isErr()) {
      return err(joinedFile.error());
    }
  }

  return ok(std::move(joined));
}

NativeSources::ResultFilesFound NativeSources::findFiles(const pkg::NativeEntry &entry) const {
  auto files = std::vector<std::string>();
  auto excluded = std::vector<std::string>();
  for (llvm::StringRef pattern : entry.sources) {
    bool isExcluded = pattern.consume_front("!");
    auto found = this->glob.findFiles(pattern);
    if (!isExcluded && found.empty()) {
      auto message = ErrorTextNativePass::noFiles(entry.name, pattern);

      return err(NativePassError{
          .code = NativePassErrorCode::NO_SOURCE_FILES,
          .message = message,
      });
    }
    auto &list = isExcluded ? excluded : files;
    list.insert(list.end(), found.begin(), found.end());
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  auto kept = std::vector<std::string>();
  std::copy_if(files.begin(), files.end(), std::back_inserter(kept),
               std::bind_front(&NativeSources::isKept, this, std::cref(excluded)));

  return ok(kept);
}

NativeSources::ResultEntryBuilt NativeSources::buildFile(const pkg::NativeEntry &entry,
                                                         llvm::StringRef file,
                                                         llvm::LLVMContext &context) const {
  auto args = this->compilerArgs(entry, file);
  auto argv = std::vector<const char *>();
  for (const std::string &arg : args) {
    argv.push_back(arg.c_str());
  }
  auto invocation = clang::createInvocation(argv);
  std::string output;
  llvm::raw_string_ostream stream(output);
  if (invocation == nullptr) {
    auto message = ErrorTextNativePass::compile(entry.name, file);

    return err(NativePassError{
        .code = NativePassErrorCode::SOURCE_ERRORS,
        .message = message,
    });
  }
  clang::CompilerInstance instance(std::move(invocation));
  instance.createVirtualFileSystem();
  auto *printer = new clang::TextDiagnosticPrinter(stream, instance.getDiagnosticOpts());
  instance.createDiagnostics(printer, true);
  clang::EmitLLVMOnlyAction action(&context);
  bool isBuilt = instance.ExecuteAction(action);
  auto built = action.takeModule();
  if (!isBuilt || built == nullptr) {
    auto message = ErrorTextNativePass::compile(entry.name, output);

    return err(NativePassError{
        .code = NativePassErrorCode::SOURCE_ERRORS,
        .message = message,
    });
  }

  return ok(std::move(built));
}

NativeSources::ResultFileJoined NativeSources::joinFile(const pkg::NativeEntry &entry,
                                                        llvm::Module &joined,
                                                        std::unique_ptr<llvm::Module> built) const {
  bool isFailed = llvm::Linker::linkModules(joined, std::move(built));
  if (isFailed) {
    auto message = ErrorTextNativePass::join(entry.name);

    return err(NativePassError{
        .code = NativePassErrorCode::JOIN_FAILED,
        .message = message,
    });
  }

  return ok();
}

std::vector<std::string> NativeSources::compilerArgs(const pkg::NativeEntry &entry,
                                                     llvm::StringRef file) const {
  auto resources = this->options.runtime.path("clang");
  auto args = std::vector<std::string>{
      resources,
      "-c",
      "-Qunused-arguments",
      "-x",
      "c",
      "-std=gnu17",
      "-w",
      "-target",
      this->target.triple().str(),
      "-resource-dir",
      resources,
  };
  if (this->target.triple().isOSWindows()) {
    args.push_back("-fms-runtime-lib=static");
  }
  args.push_back(this->options.optimize ? "-O2" : "-O0");
  if (this->options.optimize) {
    args.push_back("-Xclang");
    args.push_back("-disable-llvm-passes");
  }
  this->addDebugArgs(entry, args);
  for (const std::string &define : entry.defines) {
    args.push_back("-D" + define);
  }
  for (const std::string &include : entry.includes) {
    args.push_back("-I" + include);
  }
  args.push_back(file.str());

  return args;
}

void NativeSources::addDebugArgs(const pkg::NativeEntry &entry,
                                 std::vector<std::string> &args) const {
  bool hasLines = this->options.debug != DebugLevel::None;
  if (hasLines) {
    args.push_back(this->isDebug() ? "-g" : "-gline-tables-only");
  }
  if (hasLines && this->target.triple().isOSWindows()) {
    args.push_back("-gcodeview");
  }
  if (!this->isDebug()) {
    args.push_back("-DNDEBUG");
    return;
  }
  for (const std::string &define : entry.debugDefines) {
    args.push_back("-D" + define);
  }
}

bool NativeSources::isDebug() const {
  return this->options.debug == DebugLevel::Full;
}

bool NativeSources::isKept(const std::vector<std::string> &excluded,
                           const std::string &file) const {
  return !llvm::is_contained(excluded, file);
}

}
