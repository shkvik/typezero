#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Object literals and their spreads.
class ObjectLiteralChecker {
public:
  explicit ObjectLiteralChecker(Sema &sema);

  // Objects (sema_object.cpp). A literal takes the type expected where it is
  // written, and its fields the types of that type's fields; without one it
  // has the types of its values.
  Expr *checkObjectLit(ObjectLitExpr *lit, const Type *expected);
  // A literal with spreads, `{ ...a, x: 1 }`: the fields of all its parts.
  // `named`: whether each property names a field (a key in brackets that is
  // not a constant does not).
  Expr *checkSpreadLit(ObjectLitExpr *lit, const Type *target, llvm::ArrayRef<bool> named, bool ok);

private:
  Sema &sema_;
};

} // namespace tz
