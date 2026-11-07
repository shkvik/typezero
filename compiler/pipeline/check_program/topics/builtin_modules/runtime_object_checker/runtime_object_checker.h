#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/builtin_modules/runtime_object_checker/runtime_object_checker_errors.h"

namespace tz {

class RuntimeObjectChecker {
public:
  explicit RuntimeObjectChecker(CheckState &owner);

  Expr *checkRuntimeMember(MemberExpr *member);
  Expr *checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  const Type *checkRuntimeTarget(MemberExpr *member, bool compound);

private:
  CheckState &state;
};

}
