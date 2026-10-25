#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Functions used as values: adapters, built-in functions, async callbacks.
class FunctionValueChecker {
public:
  explicit FunctionValueChecker(Checker &checker);

  // An async function where the method uses what the function returns, for
  // which JavaScript would take the Promise: an error, and true.
  bool asyncCallbackProblem(const Expr *arg, const Checker::Signature &expected,
                            llvm::StringRef method);
  // A built-in such as Math.sqrt or parseInt where a function is expected:
  // an arrow calling it, or null if `arg` names none.
  FunctionExpr *builtinFunctionValue(Expr *arg, const Checker::Signature &expected,
                                     unsigned primary, llvm::StringRef method);
  // A function known by name (a declaration or a constant arrow) where a
  // function type that passes more or fewer arguments, or drops the result,
  // is expected: a conversion that Lowering turns into a small adapter. Null
  // if that type does not fit.
  Expr *adaptFunction(Expr *expr, const Type *to);

private:
  Checker &checker_;
};

} // namespace tz
