#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/import_binder/import_binder_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ResultTypes;
class SemanticParts;
class ModuleDeclarer;
class BuiltinModuleChecker;

class ImportBinder {
public:
  ImportBinder(SemanticState &owner, SemanticParts &all);

  void bindImports();
  void enterModule(Module *module);
  void enterHomeModule(const FunctionDecl *functionDecl);
  void bindAllImports(const std::vector<Module *> &modules);
  void resolveImports(Module *module);
  void noteImportsInFunctions(SourceFile &program);
  FunctionDecl *cycleCallee(FunctionDecl *functionDecl, llvm::StringRef name);

private:
  using ResultWholeImportOk = Result<const ImportDecl *, ImportBinderWholeImportError>;
  using ResultAliasIsUnused = Result<const ModuleName *, ImportBinderFreeNameError>;

  ResultWholeImportOk wholeImport(const ImportDecl &importDecl) const;
  ResultAliasIsUnused freeName(const ModuleState &here, const ImportDecl &importDecl,
                               const ModuleName &name) const;

  ClassDecl *nodeClass(const NodeName &nodeName) const;
  Stmt *exportedType(Module *source, llvm::StringRef name);
  void resolveImport(Module *module, const ImportDecl &importDecl);
  void bindImportedType(llvm::StringRef alias, const ModuleBinding &target);
  std::optional<ScopeEntry> exportedValue(Module *source, llvm::StringRef name);
  void importName(ModuleState &here, const ImportDecl &importDecl, const ModuleName &name);
  void reportTaken(const ModuleState &here, const ModuleName &name,
                   const ImportBinderFreeNameError &error);
  void bindImportedValue(ModuleState &here, llvm::StringRef alias, const ModuleBinding &target);

  SemanticState &state;
  ResultTypes *resultTypes = nullptr;
  ModuleDeclarer *moduleDeclarer = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
