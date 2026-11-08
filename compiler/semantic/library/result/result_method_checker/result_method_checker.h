#pragma once

#include "compiler/semantic/library/result/result_method_checker/result_method_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ResultMethodChecker {
public:
  explicit ResultMethodChecker(SemanticState &owner);

  Expr *checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static const Type *halfParam(const Type *half);
  static bool givesType(const Type *half);

  SemanticState &state;
};

}
