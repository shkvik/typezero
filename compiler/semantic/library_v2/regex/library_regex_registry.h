#pragma once

#include "compiler/program/library/regex/match_member_table/match_member_table.h"
#include "compiler/program/library/regex/regex_member_table/regex_member_table.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/regex/library_regex_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LibraryRegexRegistry {
public:
  explicit LibraryRegexRegistry(SemanticState &owner);

  void matchItems();
  void regexToString();
  void checkMatchIndex(const CheckMatchIndex &task);
  void checkRegExpCall(const CheckRegExpCall &task);
  void finishRegExpNew(const FinishRegExpNew &task);
  void checkMatchMember(const CheckMatchMember &task);
  void checkRegexMethod(const CheckRegexMethod &task);
  void checkRegexMember(const CheckRegexMember &task);
  void afterRegexRunArg(const AfterRegexRunArg &task);
  void checkRegexLiteral(const CheckRegexLiteral &task);
  void noteRegexVariable(const NoteRegexVariable &task);
  void checkRegExpStatic(const CheckRegExpStatic &task);
  void finishRegExpParse(const FinishRegExpParse &task);
  void afterRegexPattern(const AfterRegexPattern &task);
  void afterMatchPosition(const AfterMatchPosition &task);
  void afterTextReplacement(const AfterTextReplacement &task);
  void checkStringRegexMethod(const CheckStringRegexMethod &task);
  void checkRegExpConstructor(const CheckRegExpConstructor &task);
  void checkRegExpStaticMember(const CheckRegExpStaticMember &task);
  void afterFunctionReplacement(const AfterFunctionReplacement &task);

private:
  using ResultParsedFlags = Result<uint32_t, LibraryRegexRegistryParsedFlagsError>;
  using ResultGroupIndex = Result<int64_t, LibraryRegexRegistryGroupIndexError>;
  using ResultGroupsReferenced =
      Result<const std::u16string *, LibraryRegexRegistryReplacementGroupsError>;

  void checkRestLoosely(CallExpr *call);
  void compileRegexLit(RegexLitExpr *literal);
  void reportRegExpStaticMember(MemberExpr *member);
  void checkTextPattern(const AfterRegexPattern &pattern);
  void checkRegexPattern(const AfterRegexPattern &pattern);
  void checkRegexToString(CallExpr *call, const MemberExpr *member);
  void checkRegexSplit(CallExpr *call, const RegexLitExpr *regex);
  void failRegExpNew(llvm::SMLoc loc, std::vector<Expr *> *args);
  void startRegExpNew(llvm::SMLoc loc, std::vector<Expr *> *args);
  void toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) const;
  void checkRegexMatch(const AfterRegexPattern &pattern, const RegexLitExpr *regex);
  void reportNeedsGlobal(const AfterRegexPattern &pattern, const RegexLitExpr *regex);
  void checkFunctionReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex);
  void checkTextReplaceAll(CallExpr *call, const std::string &text, const std::string &escaped);
  void addNamedGroup(std::vector<Field> &fields, const uint32_t *program, uint32_t group) const;
  void checkRegexRun(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex,
                     RegexMember method);
  void checkExtraReplacerParams(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex,
                                const Type *match, Signature signature);


  bool isRegExpEscape(const Expr *expr) const;
  bool isPlainQuotedChar(char character) const;
  bool isMissingGlobalFlag(const RegexLitExpr *regex) const;
  bool hasOptionalGroups(const RegexLitExpr *regex) const;
  bool isFunctionReplacement(const Expr *replacement) const;
  bool hasNamedGroups(const uint32_t *program, uint32_t groups) const;
  bool hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  bool isAssigningName(llvm::ArrayRef<const Stmt *> stmts, llvm::StringRef name) const;
  bool isNumberedGroupKnown(const std::u16string &value, size_t at, uint32_t groups) const;
  bool isStableRegexName(llvm::StringRef name, llvm::ArrayRef<Stmt *> block, size_t index) const;
  bool hasNamedGroup(const uint32_t *program, uint32_t groups, const std::string &groupName) const;


  Expr *matchLink(Expr *object);
  Expr *itemsOfMatch(Expr *match);
  Expr *stringOfRegex(Expr *regex);
  Expr *itemsMember(Expr *match);
  Expr *optionalItems(Expr *match, Expr *items);
  Expr *regexValue(MemberExpr *member, std::optional<RegexMember> known);


  const Type *itemsType() const;
  const Type *matchType(const RegexLitExpr *regex);


  std::string argumentText(const Expr *arg) const;
  std::string staticMemberText(llvm::StringRef name) const;
  std::string matchMemberText(const MemberExpr *member) const;
  std::string escapedLiteral(const std::u16string &text) const;
  std::string groupNameAt(const std::u16string &value, size_t at) const;
  std::string groupName(const uint32_t *program, uint32_t start, uint32_t length) const;
  std::string textPatternMessage(const AfterRegexPattern &pattern, const std::string &text,
                                 const std::string &regexText, bool isWritten) const;


  ResultGroupIndex checkGroupIndex(int64_t index, int groups) const;
  ResultParsedFlags checkParsedFlags(const std::u16string &flags) const;
  ResultGroupsReferenced checkReplacementGroups(const RegexLitExpr *regex,
                                                const std::u16string &value) const;
  ResultGroupsReferenced checkGroupReference(const uint32_t *program, uint32_t groups,
                                             const std::u16string &value, size_t at) const;


  unsigned patternMinArgs(StringMethod method) const;
  std::optional<uint32_t> flagBit(llvm::StringRef name) const;
  const RegexLitExpr *knownRegex(const Expr *expr) const;
  std::vector<size_t> groupReferences(const std::u16string &value) const;
  std::optional<std::u16string> constantText(const Expr *expr) const;
  Signature replacerSignature(const RegexLitExpr *regex, const Type *match) const;
  std::u16string units(const uint32_t *program, uint32_t start, uint32_t length) const;
  std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t index) const;

  SemanticState &state;
};

}
