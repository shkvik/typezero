#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class NewChecker;
class CallChecker;
class ExprChecker;
class LiteralTypes;
class TypeRelations;
class ClassDeclarer;
class ProcessChecker;
class ArgumentChecker;
class TopLevelChecker;
class DateFormatChecker;
class IntlOptionsChecker;

enum class IntlCreation {
  NEW,
  CALL,
};

class IntlChecker {
public:
  IntlChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                     IntlCreation creation);
  Expr *checkIntlFunction(CallExpr *call, MemberExpr *member);
  Expr *checkIntlStatic(CallExpr *call, MemberExpr *member);
  Expr *checkIntlMember(MemberExpr *member, const Type *expected);
  Expr *checkLocaleList(Expr *arg, llvm::StringRef what);
  Expr *checkLocaleNew(NewExpr *expr, const Type *expected);
  Expr *checkLocaleStatic(CallExpr *call, MemberExpr *member, const Type *expected);
  bool checkLocaleArg(Expr *&arg, llvm::StringRef what);
  Expr *segmentsArray(Expr *segments);
  const Type *segmentDataType() const;
  const Type *resolvedIntlOptionsType(IntlKind kind) const;
  const Type *intlTypeNamed(llvm::StringRef name) const;

private:
  Expr *kindArg(AstContext &context, llvm::SMLoc loc, int kind) const;
  using ResultLanguageTags = Result<std::vector<std::string>, IntlCheckerLanguageTagsError>;

  std::optional<std::u16string> literalText(const Expr *expr) const;
  const IntlService *serviceNamed(llvm::StringRef name) const;
  const Type *relativePartType() const;
  const Expr *unconverted(const Expr *expr) const;
  bool isStringLike(const Type *type) const;
  std::vector<Field> resolvedDigitFields() const;
  std::vector<Field> resolvedRoundingFields() const;
  const Type *resolvedNumberOptionsType() const;
  const Type *resolvedCollatorOptionsType() const;
  const Type *resolvedPluralOptionsType() const;
  const Type *resolvedRelativeTimeOptionsType() const;
  const Type *resolvedListOptionsType() const;
  const Type *resolvedDisplayNamesOptionsType() const;
  const Type *resolvedSegmenterOptionsType() const;
  const Type *numberOptionType(llvm::StringRef name) const;

  ResultLanguageTags languageTags(const Expr *arg, llvm::StringRef what, bool constructor);

  SemanticState &state;
  NewChecker *newChecker = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  DateFormatChecker *dateFormatChecker = nullptr;
  IntlOptionsChecker *intlOptionsChecker = nullptr;
};

}
