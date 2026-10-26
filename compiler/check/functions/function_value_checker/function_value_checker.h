#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FunctionValueChecker {
public:
  explicit FunctionValueChecker(Checker &checker);

  bool asyncCallbackProblem(const Expr *arg, const Checker::Signature &expected,
                            llvm::StringRef method);
  FunctionExpr *builtinFunctionValue(Expr *arg, const Checker::Signature &expected,
                                     unsigned primary, llvm::StringRef method);
  Expr *adaptFunction(Expr *expr, const Type *to);

private:
  Checker &checker_;
};

}
