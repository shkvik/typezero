#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class WeakRefChecker {
public:
  explicit WeakRefChecker(CheckState &owner);

  const Type *resolveWeakRefType(const TypeRef &ref);
  bool checkWeakTarget(const Type *target, llvm::SMLoc loc);
  Expr *checkWeakRefNew(NewExpr *expr, const Type *expected);
  Expr *checkWeakRefMethod(CallExpr *call, MemberExpr *member);

private:
  CheckState &state;
};

}
