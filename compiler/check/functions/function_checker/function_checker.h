#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Functions: their declarations and their bodies.
class FunctionChecker {
public:
  explicit FunctionChecker(Sema &sema);

  // Declarations and statements
  // `var` is the variable of a function declared in a block. Without
  // `scoped`, a method: its name is not declared in the scope.
  void declareFunction(FunctionDecl *fn, VarSymbol *var = nullptr, bool scoped = true);
  void checkFunctionBody(FunctionDecl *fn);

private:
  Sema &sema_;
};

} // namespace tz
