#pragma once

#include "compiler/native/native_pass/native_link.h"
#include "compiler/native/native_pass/native_pass_errors.h"
#include "compiler/native/native_platform/native_platform.h"
#include "compiler/native/native_sources/native_sources.h"
#include "compiler/packages/native_entry/native_entry.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Module.h"
#include <vector>

namespace tz {

class Target;
struct BuildOptions;

class NativePass {
public:
  using ResultNativeLinked = Result<NativeLink, std::vector<Diagnostic>>;

  NativePass(const Target &target, const BuildOptions &options);

  ResultNativeLinked run(llvm::Module &module, llvm::ArrayRef<ForeignFunction> foreign) const;

private:
  using ResultEntriesFound = Result<std::vector<const pkg::NativeEntry *>, NativePassError>;
  using ResultSymbolsFound = Result<llvm::StringSet<>, NativePassError>;
  using ResultEntryChecked = Result<void, NativePassError>;
  using ResultLicenseFound = Result<NativeLicense, NativePassError>;

  ResultEntriesFound checkEntriesFound() const;
  ResultSymbolsFound readLibrarySymbols(const pkg::NativeEntry &entry) const;
  ResultSymbolsFound readArchiveSymbols(const pkg::NativeEntry &entry, llvm::StringRef path) const;
  ResultEntryChecked checkEntryFiles(const pkg::NativeEntry &entry) const;
  ResultLicenseFound readLicense(const pkg::NativeEntry &entry) const;
  ResultEntryChecked linkEntry(const pkg::NativeEntry &entry, const llvm::StringSet<> &used,
                               llvm::Module &module, NativeLink &link) const;
  ResultEntryChecked linkSources(const pkg::NativeEntry &entry, const llvm::StringSet<> &used,
                                 llvm::Module &module, NativeLink &link) const;
  ResultEntryChecked joinCode(const pkg::NativeEntry &entry, llvm::Module &module,
                              std::unique_ptr<llvm::Module> code) const;

  llvm::StringSet<> usedNames(const llvm::Module &module,
                              llvm::ArrayRef<ForeignFunction> foreign) const;
  bool isProviding(const llvm::StringSet<> &symbols, const llvm::StringSet<> &used) const;
  bool isDefining(const llvm::Module &code, const llvm::StringSet<> &used) const;
  void addEntry(const pkg::NativeEntry &entry, NativeLicense license, NativeLink &link) const;
  std::vector<Diagnostic> toolErrors(const NativePassError &error) const;

  const Target &target;
  const BuildOptions &options;
  NativePlatform platform;
  NativeSources sources;
};

}
