#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TupleChecker {
public:
  explicit TupleChecker(Checker &checker);

  Expr *checkTupleMember(MemberExpr *member);
  Expr *checkTupleMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTupleLit(ArrayLitExpr *lit, const Type *tuple);
  static const Type *tupleElement(const Type *tuple);
  Expr *checkTupleJoin(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
