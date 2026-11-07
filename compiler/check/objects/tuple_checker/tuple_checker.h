#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/objects/tuple_checker/tuple_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class TupleChecker {
public:
  using ResultTupleMember = Result<Expr *, TupleCheckerCheckTupleMemberError>;
  using ResultTupleMethod = Result<Expr *, TupleCheckerCheckTupleMethodError>;
  using ResultTupleFilled = Result<Expr *, TupleCheckerCheckTupleLitError>;

  explicit TupleChecker(Checker &owner);

  Expr *arrayCopy(Expr *tuple);
  static const Type *tupleElement(const Type *tuple);
  ResultTupleMember checkTupleMember(MemberExpr *member);
  ResultTupleFilled checkTupleLit(ArrayLitExpr *lit, const Type *tuple);
  ResultTupleMethod checkTupleMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultTupleJoined = Result<Expr *, TupleCheckerCheckTupleJoinError>;

  ResultTupleJoined checkTupleJoin(CallExpr *call, MemberExpr *member);

  Checker &checker;
};

}
