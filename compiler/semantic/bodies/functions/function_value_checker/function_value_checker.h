#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FunctionValueChecker {
public:
  using ResultAsyncCallbackOk = Result<const Expr *, FunctionValueCheckerAsyncCallbackError>;

  explicit FunctionValueChecker(SemanticState &owner);

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

  SemanticState &state;
};

}
