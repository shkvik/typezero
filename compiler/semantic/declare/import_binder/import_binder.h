#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/import_binder/import_binder_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ImportBinder {
public:
  explicit ImportBinder(SemanticState &owner);

  void bindImports();
  void resolveImports(Module *module);
  void noteImportsInFunctions(SourceFile &program);
  FunctionDecl *cycleCallee(FunctionDecl *fn, llvm::StringRef name);

private:
  Stmt *exportedType(Module *source, llvm::StringRef name);
  std::optional<ScopeEntry> exportedValue(Module *source, llvm::StringRef name);
  using ResultWholeImportOk = Result<const ImportDecl *, ImportBinderWholeImportError>;
  using ResultAliasIsUnused = Result<const ModuleName *, ImportBinderFreeNameError>;

  static ResultWholeImportOk wholeImport(const ImportDecl &decl);
  static ResultAliasIsUnused freeName(const ModuleState &here, const ImportDecl &decl,
                                      const ModuleName &name);

  void reportTaken(const ModuleState &here, const ModuleName &name,
                   const ImportBinderFreeNameError &error);
  void importName(ModuleState &here, const ImportDecl &decl, const ModuleName &name);

  SemanticState &state;
};

}
