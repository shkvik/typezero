#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class FunctionValueChecker {
public:
  using ResultAsyncCallbackOk = Result<const Expr *, FunctionValueCheckerAsyncCallbackError>;

  explicit FunctionValueChecker(Checker &owner);

  ResultAsyncCallbackOk asyncCallback(const Expr *arg, const Checker::Signature &expected,
                                      llvm::StringRef method) const;
  FunctionExpr *builtinFunctionValue(Expr *arg, const Checker::Signature &expected,
                                     unsigned primary, llvm::StringRef method);
  Expr *adaptFunction(Expr *expr, const Type *to);

private:
  using ResultArgumentsMatch = Result<size_t, FunctionValueCheckerExtraArgumentsError>;

  static ResultArgumentsMatch extraArguments(const Expr *arg, llvm::StringRef name,
                                             llvm::StringRef method, size_t count,
                                             unsigned primary);
  static const char *callbackArgs(llvm::StringRef method);
  static const char *ordinal(size_t i);
  static const char *callbackArgName(llvm::StringRef method, size_t i);

  Checker &checker;
};

}
