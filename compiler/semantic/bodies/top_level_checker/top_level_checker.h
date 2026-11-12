#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class StmtChecker;
class ImportBinder;
class EmitterChecker;
class ModuleDeclarer;
class ReturnInferrer;

class TopLevelChecker {
public:
  using ResultSourceIsLoaded = Result<const NameExpr *, TopLevelCheckerCheckLoadedError>;
  using ResultVarNotImported = Result<const VarSymbol *, TopLevelCheckerCheckImportAssignError>;

  TopLevelChecker(SemanticState &owner, SemanticParts &all);

  ScopeEntry lookupValue(llvm::StringRef name);
  bool loadEarly(Module *source, llvm::StringRef name);
  ResultSourceIsLoaded checkLoaded(const NameExpr *name);
  void loadTopLevel(Module *module, llvm::StringRef until = {});
  void loadAllTopLevels(const std::vector<Module *> &modules);
  Module *laterSource(llvm::StringRef name, llvm::StringRef &declared);
  ResultVarNotImported checkImportAssign(const NameExpr *name, const VarSymbol *variable) const;

private:
  std::string loadLabel(llvm::StringRef name, llvm::StringRef declared, const Module *source) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  StmtChecker *stmtChecker = nullptr;
  ImportBinder *importBinder = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ModuleDeclarer *moduleDeclarer = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
};

}
