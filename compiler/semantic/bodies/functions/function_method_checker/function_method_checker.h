#pragma once

#include "compiler/program/library/function/function_method_table/function_method_table.h"
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
  using ResultThisMethodKnown = Result<FunctionMethod, FunctionMethodCheckerCheckThisCallError>;
  using ResultThisArgumentsFit = Result<const CallExpr *, FunctionMethodCheckerCheckThisCallError>;

  ResultThisMethodKnown checkThisMethod(CallExpr *call, const MemberExpr *member);
  ResultThisArgumentsFit checkThisArguments(CallExpr *call, const MemberExpr *member,
                                            FunctionMethod method);
  Expr *checkThisArgumentTypes(CallExpr *call, MemberExpr *member, FunctionMethod method);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
