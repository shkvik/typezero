#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Object literals with methods, and the interfaces they implement.
class MethodLiteralChecker {
public:
  explicit MethodLiteralChecker(Sema &sema);

  // An object literal with methods: an instance of a class of its own. Where
  // an interface is expected, its fields and methods take their types from
  // it, and the class implements it.
  Expr *checkMethodLiteral(ObjectLitExpr *lit, const Type *target);
  bool implementsLiteral(ObjectLitExpr *lit, const Type *target);

private:
  Sema &sema_;
};

} // namespace tz
