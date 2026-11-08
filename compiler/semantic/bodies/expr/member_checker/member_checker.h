#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class MemberChecker {
public:
  using ResultIndexedElement = Result<Expr *, MemberCheckerCheckIndexError>;
  using ResultMemberReadable = Result<Expr *, MemberCheckerCheckMemberError>;

  explicit MemberChecker(SemanticState &owner);

  Expr *checkPosition(Expr *expr);
  ResultIndexedElement checkIndex(IndexExpr *index);
  ResultMemberReadable checkMember(MemberExpr *member, const Type *expected);

private:
  using ResultIndexIsInteger = Result<const Type *, MemberCheckerPositionTypeError>;

  static ResultIndexIsInteger positionType(const Type *type);

  SemanticState &state;
};

}
