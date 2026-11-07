#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ResultMethodChecker {
public:
  explicit ResultMethodChecker(CheckState &owner);

  Expr *checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static const Type *halfParam(const Type *half);
  static bool givesType(const Type *half);

  CheckState &state;
};

}
