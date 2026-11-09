#pragma once

#include "compiler/semantic/library/regex/string_regex_checker/string_regex_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class ExprChecker;
class FlowChecker;
class RegexChecker;
class MemberChecker;
class ArgumentChecker;
class CallbackChecker;

class StringRegexChecker {
public:
  StringRegexChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkStringRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex);

private:
  static std::u16string units(const uint32_t *program, uint32_t start, uint32_t length);
  static std::string escapedLiteral(const std::u16string &text);
  static std::string argumentText(const Expr *arg);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  RegexChecker *regexChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
