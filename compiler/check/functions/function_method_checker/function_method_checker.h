#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/functions/function_method_checker/function_method_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class FunctionMethodChecker {
public:
  using ResultThisCallMade = Result<Expr *, FunctionMethodCheckerCheckThisCallError>;
  using ResultParamsJoined = Result<Expr *, FunctionMethodCheckerCheckListMethodError>;

  explicit FunctionMethodChecker(Checker &owner);

  ResultThisCallMade checkThisCall(CallExpr *call, MemberExpr *member);
  ResultParamsJoined checkListMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker;
};

}
