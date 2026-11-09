#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/functions/function_method_checker/function_method_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ArgListChecker;
class ArgumentChecker;

class FunctionMethodChecker {
public:
  using ResultThisCallMade = Result<Expr *, FunctionMethodCheckerCheckThisCallError>;
  using ResultParamsJoined = Result<Expr *, FunctionMethodCheckerCheckListMethodError>;

  FunctionMethodChecker(SemanticState &owner, SemanticParts &all);

  ResultThisCallMade checkThisCall(CallExpr *call, MemberExpr *member);
  ResultParamsJoined checkListMethod(CallExpr *call, MemberExpr *member);

private:
  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
