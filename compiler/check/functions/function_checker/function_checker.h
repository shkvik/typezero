#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Functions: their declarations and their bodies.
class FunctionChecker {
public:
  explicit FunctionChecker(Checker &checker);

  // Declarations and statements
  // `var` is the variable of a function declared in a block. Without
  // `scoped`, a method: its name is not declared in the scope.
  void declareFunction(FunctionDecl *fn, VarSymbol *var = nullptr, bool scoped = true);
  void checkFunctionBody(FunctionDecl *fn);

private:
  Checker &checker_;
};

} // namespace tz
