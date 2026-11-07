#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/top_level_checker/top_level_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class TopLevelChecker {
public:
  using ResultSourceIsLoaded = Result<const NameExpr *, TopLevelCheckerCheckLoadedError>;
  using ResultVarNotImported = Result<const VarSymbol *, TopLevelCheckerCheckImportAssignError>;

  explicit TopLevelChecker(Checker &owner);

  ScopeEntry lookupValue(llvm::StringRef name);
  bool loadEarly(Module *source, llvm::StringRef name);
  ResultSourceIsLoaded checkLoaded(const NameExpr *name);
  void checkTopLevel(Module *module, llvm::StringRef until = {});
  Module *laterSource(llvm::StringRef name, llvm::StringRef &declared);
  static std::string loadChain(const Module *reader, const Module *source);
  ResultVarNotImported checkImportAssign(const NameExpr *name, const VarSymbol *var) const;

private:
  std::string loadLabel(llvm::StringRef name, llvm::StringRef declared, const Module *source) const;

  Checker &checker;
};

}
