#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class TopLevelChecker {
public:
  using ResultSourceIsLoaded = Result<const NameExpr *, TopLevelCheckerCheckLoadedError>;
  using ResultVarNotImported = Result<const VarSymbol *, TopLevelCheckerCheckImportAssignError>;

  explicit TopLevelChecker(SemanticState &owner);

  ScopeEntry lookupValue(llvm::StringRef name);
  bool loadEarly(Module *source, llvm::StringRef name);
  ResultSourceIsLoaded checkLoaded(const NameExpr *name);
  void loadTopLevel(Module *module, llvm::StringRef until = {});
  Module *laterSource(llvm::StringRef name, llvm::StringRef &declared);
  static std::string loadChain(const Module *reader, const Module *source);
  ResultVarNotImported checkImportAssign(const NameExpr *name, const VarSymbol *var) const;

private:
  std::string loadLabel(llvm::StringRef name, llvm::StringRef declared, const Module *source) const;

  SemanticState &state;
};

}
