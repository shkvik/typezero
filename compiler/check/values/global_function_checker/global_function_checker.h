#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/values/global_function_checker/global_function_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class GlobalFunctionChecker {
public:
  using ResultGlobalCallMade = Result<Expr *, GlobalFunctionCheckerCheckGlobalCallError>;
  using ResultFunctionCalled = Result<Expr *, GlobalFunctionCheckerCheckGlobalFunctionError>;

  explicit GlobalFunctionChecker(Checker &owner);

  ResultGlobalCallMade checkGlobalCall(CallExpr *call, NameExpr *name);
  ResultFunctionCalled checkGlobalFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker;
};

}
