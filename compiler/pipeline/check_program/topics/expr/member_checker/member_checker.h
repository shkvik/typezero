#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class MemberChecker {
public:
  using ResultIndexedElement = Result<Expr *, MemberCheckerCheckIndexError>;
  using ResultMemberReadable = Result<Expr *, MemberCheckerCheckMemberError>;

  explicit MemberChecker(CheckState &owner);

  Expr *checkPosition(Expr *expr);
  ResultIndexedElement checkIndex(IndexExpr *index);
  ResultMemberReadable checkMember(MemberExpr *member, const Type *expected);

private:
  using ResultIndexIsInteger = Result<const Type *, MemberCheckerPositionTypeError>;

  static ResultIndexIsInteger positionType(const Type *type);

  CheckState &state;
};

}
