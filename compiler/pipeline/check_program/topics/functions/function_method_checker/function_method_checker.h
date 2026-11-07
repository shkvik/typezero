#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/functions/function_method_checker/function_method_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class FunctionMethodChecker {
public:
  using ResultThisCallMade = Result<Expr *, FunctionMethodCheckerCheckThisCallError>;
  using ResultParamsJoined = Result<Expr *, FunctionMethodCheckerCheckListMethodError>;

  explicit FunctionMethodChecker(CheckState &owner);

  ResultThisCallMade checkThisCall(CallExpr *call, MemberExpr *member);
  ResultParamsJoined checkListMethod(CallExpr *call, MemberExpr *member);

private:
  CheckState &state;
};

}
