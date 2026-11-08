#pragma once

#include "compiler/semantic/library/regex/string_regex_checker/string_regex_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class StringRegexChecker {
public:
  explicit StringRegexChecker(SemanticState &owner);

  Expr *checkStringRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex);

private:
  static std::u16string units(const uint32_t *program, uint32_t start, uint32_t length);
  static std::string escapedLiteral(const std::u16string &text);
  static std::string argumentText(const Expr *arg);

  SemanticState &state;
};

}
