#pragma once

#include "compiler/native/native_glob/native_glob.h"
#include "compiler/native/native_pass/native_pass_errors.h"
#include "compiler/packages/native_entry/native_entry.h"
#include "compiler/result/result/result.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <memory>
#include <string>
#include <vector>

namespace tz {

class Target;
struct BuildOptions;

class NativeSources {
public:
  using ResultEntryBuilt = Result<std::unique_ptr<llvm::Module>, NativePassError>;

  NativeSources(const Target &target, const BuildOptions &options);

  ResultEntryBuilt buildEntry(const pkg::NativeEntry &entry, llvm::LLVMContext &context) const;

private:
  using ResultFilesFound = Result<std::vector<std::string>, NativePassError>;
  using ResultFileJoined = Result<void, NativePassError>;

  ResultFilesFound findFiles(const pkg::NativeEntry &entry) const;
  ResultEntryBuilt buildFile(const pkg::NativeEntry &entry, llvm::StringRef file,
                             llvm::LLVMContext &context) const;
  ResultFileJoined joinFile(const pkg::NativeEntry &entry, llvm::Module &joined,
                            std::unique_ptr<llvm::Module> built) const;
  std::vector<std::string> compilerArgs(const pkg::NativeEntry &entry, llvm::StringRef file) const;
  void addDebugArgs(const pkg::NativeEntry &entry, std::vector<std::string> &args) const;
  bool isDebug() const;
  bool isKept(const std::vector<std::string> &excluded, const std::string &file) const;

  const Target &target;
  const BuildOptions &options;
  NativeGlob glob;
};

}
