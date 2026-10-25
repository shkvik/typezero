#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of strings that take a regular expression, and their replacements.
class StringRegexChecker {
public:
  explicit StringRegexChecker(Sema &sema);

  // The methods of strings that take a regex: match, matchAll, search,
  // replace, replaceAll and split. Null for other methods.
  Expr *checkStringRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex);

private:
  Sema &sema_;
};

} // namespace tz
