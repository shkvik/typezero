#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/regex/match_checker/match_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class MatchChecker {
public:
  explicit MatchChecker(CheckState &owner);

  Expr *checkMatchMember(MemberExpr *member);
  Expr *checkMatchIndex(IndexExpr *index);
  Expr *matchItems(Expr *match);

private:
  using ResultGroupIndex = Result<int64_t, MatchCheckerGroupIndexError>;

  static ResultGroupIndex groupIndex(int64_t index, int groups);

  CheckState &state;
};

}
