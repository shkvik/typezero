#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// WeakRef (docs/objects.md, section 11).
class WeakRefChecker {
public:
  explicit WeakRefChecker(Checker &checker);

  // WeakRef (sema_weak.cpp, docs/objects.md section 11): `WeakRef<T>` of an
  // object type T, `new WeakRef(x)` and `w.deref()`.
  const Type *resolveWeakRefType(const TypeRef &ref);
  bool checkWeakTarget(const Type *target, llvm::SMLoc loc);
  Expr *checkWeakRefNew(NewExpr *expr, const Type *expected);
  Expr *checkWeakRefMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
