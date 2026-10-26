#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class MatchChecker {
public:
  explicit MatchChecker(Checker &checker);

  Expr *checkMatchMember(MemberExpr *member);
  Expr *checkMatchIndex(IndexExpr *index);
  Expr *matchItems(Expr *match);

private:
  Checker &checker_;
};

}
