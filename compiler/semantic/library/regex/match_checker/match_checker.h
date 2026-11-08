#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/regex/match_checker/match_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class MatchChecker {
public:
  explicit MatchChecker(SemanticState &owner);

  Expr *checkMatchMember(MemberExpr *member);
  Expr *checkMatchIndex(IndexExpr *index);
  Expr *matchItems(Expr *match);

private:
  using ResultGroupIndex = Result<int64_t, MatchCheckerGroupIndexError>;

  static ResultGroupIndex groupIndex(int64_t index, int groups);

  SemanticState &state;
};

}
