#pragma once

#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class RuntimeObjectChecker {
public:
  explicit RuntimeObjectChecker(SemanticState &owner);

  Expr *checkRuntimeMember(MemberExpr *member);
  Expr *checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  const Type *checkRuntimeTarget(MemberExpr *member, bool compound);

private:
  SemanticState &state;
};

}
