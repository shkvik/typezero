#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class RuntimeObjectChecker {
public:
  explicit RuntimeObjectChecker(CheckState &owner);

  Expr *checkRuntimeMember(MemberExpr *member);
  Expr *checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  const Type *checkRuntimeTarget(MemberExpr *member, bool compound);
  static std::string runtimeMembers(const Type *type);

private:
  CheckState &state;
};

}
