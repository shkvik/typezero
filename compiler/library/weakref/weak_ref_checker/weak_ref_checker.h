#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// WeakRef (docs/objects.md, section 11).
class WeakRefChecker {
public:
  explicit WeakRefChecker(Sema &sema);

  // WeakRef (sema_weak.cpp, docs/objects.md section 11): `WeakRef<T>` of an
  // object type T, `new WeakRef(x)` and `w.deref()`.
  const Type *resolveWeakRefType(const TypeRef &ref);
  bool checkWeakTarget(const Type *target, llvm::SMLoc loc);
  Expr *checkWeakRefNew(NewExpr *expr, const Type *expected);
  Expr *checkWeakRefMethod(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
