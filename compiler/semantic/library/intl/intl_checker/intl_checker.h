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
class ProcessChecker;
class ArgumentChecker;
class TopLevelChecker;
class IntlOptionsChecker;

class IntlChecker {
public:
  IntlChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                     bool withNew);
  Expr *checkIntlFunction(CallExpr *call, MemberExpr *member);
  Expr *checkIntlStatic(CallExpr *call, MemberExpr *member);
  Expr *checkIntlMember(MemberExpr *member, const Type *expected);
  Expr *checkLocaleList(Expr *arg, llvm::StringRef what);
  Expr *checkLocaleNew(NewExpr *expr, const Type *expected);
  Expr *checkLocaleStatic(CallExpr *call, MemberExpr *member, const Type *expected);
  bool checkLocaleArg(Expr *&arg, llvm::StringRef what);
  Expr *segmentsArray(Expr *segments);
  static const Type *segmentDataType();
  static const Type *resolvedIntlOptionsType(IntlKind kind);
  static const Type *intlTypeNamed(llvm::StringRef name);

private:
  static Expr *kindArg(AstContext &ctx, llvm::SMLoc loc, int kind);
  using ResultLanguageTags = Result<std::vector<std::string>, IntlCheckerLanguageTagsError>;

  static std::optional<std::u16string> literalText(const Expr *e);
  static const IntlService *serviceNamed(llvm::StringRef name);
  static std::u16string utf16(llvm::StringRef text);
  static const Type *relativePartType();
  static const Expr *unconverted(const Expr *expr);
  static bool isStringLike(const Type *type);
  static std::vector<Field> resolvedDigitFields();
  static std::vector<Field> resolvedRoundingFields();
  static const Type *resolvedNumberOptionsType();
  static const Type *resolvedCollatorOptionsType();
  static const Type *resolvedPluralOptionsType();
  static const Type *resolvedRelativeTimeOptionsType();
  static const Type *resolvedListOptionsType();
  static const Type *resolvedDisplayNamesOptionsType();
  static const Type *resolvedSegmenterOptionsType();
  static const Type *numberOptionType(llvm::StringRef name);

  ResultLanguageTags languageTags(const Expr *arg, llvm::StringRef what, bool constructor);

  SemanticState &state;
  NewChecker *newChecker = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeRelations *typeRelations = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  IntlOptionsChecker *intlOptionsChecker = nullptr;
};

}
