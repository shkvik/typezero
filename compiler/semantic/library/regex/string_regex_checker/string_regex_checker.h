#pragma once

#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/regex/string_regex_checker/string_regex_checker_errors.h"
#include "compiler/semantic/semantic_state/function_stack/function_stack_types.h"
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
  struct PatternCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    StringMethod method = StringMethod::SPLIT;
  };

  using ResultGroupsReferenced =
      Result<const std::u16string *, StringRegexCheckerReplacementGroupsError>;

  std::string escapedLiteral(const std::u16string &text) const;
  std::string argumentText(const Expr *arg) const;
  bool isMissingGlobalFlag(const RegexLitExpr *regex) const;
  bool isPlainQuotedChar(char character) const;
  unsigned patternMinArgs(StringMethod method) const;
  bool hasOptionalGroups(const RegexLitExpr *regex) const;
  std::vector<size_t> groupReferences(const std::u16string &value) const;
  ResultGroupsReferenced checkGroupReference(const uint32_t *program, uint32_t groups,
                                             const std::u16string &value, size_t at) const;
  std::string groupName(const uint32_t *program, uint32_t start, uint32_t length) const;
  Expr *toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) const;
  std::string groupNameAt(const std::u16string &value, size_t at) const;
  bool hasNamedGroups(const uint32_t *program, uint32_t groups) const;
  bool isNumberedGroupKnown(const std::u16string &value, size_t at, uint32_t groups) const;
  bool hasNamedGroup(const uint32_t *program, uint32_t groups, const std::string &groupName) const;
  Signature replacerSignature(const RegexLitExpr *regex, const Type *match) const;
  ResultGroupsReferenced checkReplacementGroups(const RegexLitExpr *regex,
                                                const std::u16string &value) const;

  Expr *checkRestLoosely(CallExpr *call);
  Expr *checkTextPattern(const PatternCall &pattern);
  Expr *checkRegexPattern(const PatternCall &pattern);
  bool isFunctionReplacement(const Expr *replacement) const;
  Expr *checkRegexSplit(CallExpr *call, const RegexLitExpr *regex);
  Expr *checkTextReplacement(CallExpr *call, const RegexLitExpr *regex);
  Expr *checkRegexMatch(const PatternCall &pattern, const RegexLitExpr *regex);
  Expr *reportNeedsGlobal(const PatternCall &pattern, const RegexLitExpr *regex);
  Expr *checkTextReplaceAll(CallExpr *call, const std::string &text, const std::string &escaped);
  Expr *checkFunctionReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex);
  Expr *checkExtraReplacerParams(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex,
                                 const Type *match, Signature signature);

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
