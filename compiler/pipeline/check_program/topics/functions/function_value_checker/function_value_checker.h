#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/functions/function_value_checker/function_value_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class FunctionValueChecker {
public:
  using ResultAsyncCallbackOk = Result<const Expr *, FunctionValueCheckerAsyncCallbackError>;

  explicit FunctionValueChecker(CheckState &owner);

  ResultAsyncCallbackOk asyncCallback(const Expr *arg, const Signature &expected,
                                      llvm::StringRef method) const;
  FunctionExpr *builtinFunctionValue(Expr *arg, const Signature &expected, unsigned primary,
                                     llvm::StringRef method);
  Expr *adaptFunction(Expr *expr, const Type *to);

private:
  using ResultArgumentsMatch = Result<size_t, FunctionValueCheckerExtraArgumentsError>;

  static ResultArgumentsMatch extraArguments(const Expr *arg, llvm::StringRef name,
                                             llvm::StringRef method, size_t count,
                                             unsigned primary);

  CheckState &state;
};

}
