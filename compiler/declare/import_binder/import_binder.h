#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/declare/import_binder/import_binder_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ImportBinder {
public:
  explicit ImportBinder(Checker &owner);

  void bindImports();
  void resolveImports(Module *module);
  void noteImportsInFunctions(SourceFile &program);
  FunctionDecl *cycleCallee(FunctionDecl *fn, llvm::StringRef name);

private:
  using ResultWholeImportOk = Result<const ImportDecl *, ImportBinderWholeImportError>;
  using ResultAliasIsUnused = Result<const ModuleName *, ImportBinderFreeNameError>;

  static ResultWholeImportOk wholeImport(const ImportDecl &decl);
  static ResultAliasIsUnused freeName(const Checker::ModuleState &here, const ImportDecl &decl,
                                      const ModuleName &name);

  void reportTaken(const Checker::ModuleState &here, const ModuleName &name,
                   const ImportBinderFreeNameError &error);
  void importName(Checker::ModuleState &here, const ImportDecl &decl, const ModuleName &name);

  Checker &checker;
};

}
