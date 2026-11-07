#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/member_checker/member_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class MemberChecker {
public:
  using ResultIndexedElement = Result<Expr *, MemberCheckerCheckIndexError>;
  using ResultMemberReadable = Result<Expr *, MemberCheckerCheckMemberError>;

  explicit MemberChecker(Checker &owner);

  Expr *checkPosition(Expr *expr);
  ResultIndexedElement checkIndex(IndexExpr *index);
  ResultMemberReadable checkMember(MemberExpr *member, const Type *expected);

private:
  using ResultIndexIsInteger = Result<const Type *, MemberCheckerPositionTypeError>;

  static ResultIndexIsInteger positionType(const Type *type);

  Checker &checker;
};

}
