#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/global_function_checker/global_function_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class GlobalFunctionChecker {
public:
  using ResultGlobalCallMade = Result<Expr *, GlobalFunctionCheckerCheckGlobalCallError>;
  using ResultFunctionCalled = Result<Expr *, GlobalFunctionCheckerCheckGlobalFunctionError>;

  explicit GlobalFunctionChecker(SemanticState &owner);

  ResultGlobalCallMade checkGlobalCall(CallExpr *call, NameExpr *name);
  ResultFunctionCalled checkGlobalFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  SemanticState &state;
};

}
