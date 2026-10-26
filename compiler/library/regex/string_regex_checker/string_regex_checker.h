#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class StringRegexChecker {
public:
  explicit StringRegexChecker(Checker &checker);

  Expr *checkStringRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex);

private:
  Checker &checker_;
};

}
