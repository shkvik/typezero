#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/values/number_methods/number_methods_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class NumberMethods {
public:
  using ResultStaticCallMade = Result<Expr *, NumberMethodsCheckStaticCallError>;
  using ResultMethodCallMade = Result<Expr *, NumberMethodsCheckNumberMethodError>;

  explicit NumberMethods(CheckState &owner);

  ResultStaticCallMade checkStaticCall(CallExpr *call, MemberExpr *member);
  ResultMethodCallMade checkNumberMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultConversionMade = Result<Expr *, NumberMethodsCheckConversionError>;

  ResultConversionMade checkConversion(CallExpr *call, MemberExpr *member);

  CheckState &state;
};

}
