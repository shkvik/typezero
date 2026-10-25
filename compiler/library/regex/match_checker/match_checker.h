#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Matches: their captures and groups.
class MatchChecker {
public:
  explicit MatchChecker(Checker &checker);

  // A match: index, input, groups and length; m[k]; and the match as the
  // array of its captures, for its methods, for...of, spreading and
  // `m ?? []` (of a `T | undefined` match, an optional array).
  Expr *checkMatchMember(MemberExpr *member);
  Expr *checkMatchIndex(IndexExpr *index);
  Expr *matchItems(Expr *match);

private:
  Checker &checker_;
};

} // namespace tz
