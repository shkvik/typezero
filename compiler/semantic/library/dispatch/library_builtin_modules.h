#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/dispatch/library_dispatch_registry_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class SemanticState;

class LibraryBuiltinModules {
public:
  using ResultModuleFound = Result<NodeModule, LibraryModulesModuleFoundError>;
  using ResultExportFound = Result<llvm::StringRef, LibraryModulesExportFoundError>;

  explicit LibraryBuiltinModules(SemanticState &owner);

  ResultExportFound checkOsExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkFsExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkTzFsExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkTzIoExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkPathExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkChildExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkBufferExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkCryptoExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkTimersExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkProcessExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkPromisesExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkTzCryptoExport(llvm::StringRef name, llvm::StringRef path) const;
  ResultExportFound checkReadlineExport(llvm::StringRef name, llvm::StringRef path,
                                        NodeModule module) const;
  ResultExportFound checkModuleExport(NodeModule module, llvm::StringRef name,
                                      llvm::StringRef path) const;


  ResultModuleFound checkExportFound(NodeModule module, llvm::StringRef name,
                                     llvm::StringRef path) const;
  TypeAliasDecl *findExported(NodeModule module, llvm::StringRef name) const;

private:
  SemanticState &state;
};

}
