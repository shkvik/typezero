#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Tuples: literals, elements and methods.
class TupleChecker {
public:
  explicit TupleChecker(Sema &sema);

  // Tuples: `t.length`, and the methods of arrays, which tuples lack.
  Expr *checkTupleMember(MemberExpr *member);
  Expr *checkTupleMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTupleLit(ArrayLitExpr *lit, const Type *tuple);
  // The type one array holds the elements of a tuple in, or null.
  static const Type *tupleElement(const Type *tuple);
  Expr *checkTupleJoin(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
