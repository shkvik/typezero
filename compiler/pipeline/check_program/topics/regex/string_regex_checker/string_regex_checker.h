#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/regex/string_regex_checker/string_regex_checker_errors.h"

namespace tz {

class StringRegexChecker {
public:
  explicit StringRegexChecker(CheckState &owner);

  Expr *checkStringRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex);

private:
  static std::u16string units(const uint32_t *program, uint32_t start, uint32_t length);
  static std::string escapedLiteral(const std::u16string &text);
  static std::string argumentText(const Expr *arg);

  CheckState &state;
};

}
