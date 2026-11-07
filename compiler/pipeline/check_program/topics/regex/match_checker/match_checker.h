#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class MatchChecker {
public:
  explicit MatchChecker(CheckState &owner);

  Expr *checkMatchMember(MemberExpr *member);
  Expr *checkMatchIndex(IndexExpr *index);
  Expr *matchItems(Expr *match);

private:
  CheckState &state;
};

}
