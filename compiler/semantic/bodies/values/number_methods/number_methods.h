#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/number_methods/number_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class NumberMethods {
public:
  using ResultStaticCallMade = Result<Expr *, NumberMethodsCheckStaticCallError>;
  using ResultMethodCallMade = Result<Expr *, NumberMethodsCheckNumberMethodError>;

  explicit NumberMethods(SemanticState &owner);

  ResultStaticCallMade checkStaticCall(CallExpr *call, MemberExpr *member);
  ResultMethodCallMade checkNumberMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultConversionMade = Result<Expr *, NumberMethodsCheckConversionError>;

  ResultConversionMade checkConversion(CallExpr *call, MemberExpr *member);

  SemanticState &state;
};

}
