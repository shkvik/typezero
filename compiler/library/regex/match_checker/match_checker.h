#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Matches: their captures and groups.
class MatchChecker {
public:
  explicit MatchChecker(Sema &sema);

  // A match: index, input, groups and length; m[k]; and the match as the
  // array of its captures, for its methods, for...of, spreading and
  // `m ?? []` (of a `T | undefined` match, an optional array).
  Expr *checkMatchMember(MemberExpr *member);
  Expr *checkMatchIndex(IndexExpr *index);
  Expr *matchItems(Expr *match);

private:
  Sema &sema_;
};

} // namespace tz
