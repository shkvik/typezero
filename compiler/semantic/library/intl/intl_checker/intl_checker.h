#pragma once

#include "compiler/program/library/intl/intl_service_table/intl_service_table.h"
#include "compiler/program/library/intl/intl_shape_table/intl_shape_table.h"
#include "compiler/result/result/result.h"
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
  bool prepareLocaleArg(Expr *&arg, llvm::StringRef what);
  Expr *segmentsArray(Expr *segments);
  const Type *segmentDataType() const;
  const Type *shapeType(IntlShape shape) const;
  const Type *resolvedIntlOptionsType(IntlKind kind) const;
  const Type *intlTypeNamed(llvm::StringRef name) const;

private:
  Expr *kindArg(AstContext &context, llvm::SMLoc loc, int kind) const;
  using ResultLanguageTags = Result<std::vector<std::string>, IntlCheckerLanguageTagsError>;

  std::optional<std::u16string> literalText(const Expr *expr) const;
  const Expr *unconverted(const Expr *expr) const;
  bool isStringLike(const Type *type) const;
  const Type *shapeValueType(IntlValueType type) const;
  const Type *numberOptionType(IntlOptionKey key) const;

  enum class LanguageTagUse {
    LOCALE_CONSTRUCTOR,
    LOCALE_ARGUMENT,
  };

  struct WantedCallback {
    const Type *param;
    const Type *ret;
  };

  ResultLanguageTags checkLanguageTags(const Expr *arg, llvm::StringRef what, LanguageTagUse use);
  bool isStringLiteralMember(const Type *member) const;
  std::vector<int64_t> digitRange(IntlOptionKey key) const;
  const Type *serviceTypeNamed(llvm::StringRef name) const;
  const Type *serviceOptionsType(const IntlService &service) const;
  const Type *optionType(const IntlOption &option) const;
  const Type *listedOptionType(const IntlOption &option) const;
  Expr *failIntlNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  void walkLocaleElements(const ArrayLitExpr *list, llvm::StringRef what);
  std::vector<Expr *> *constructedArgs(NewExpr *made) const;
  void walkArgsLoosely(std::vector<Expr *> &args);
  Expr *checkCanonicalLocales(CallExpr *call, MemberExpr *member);
  bool prepareStaticOptions(CallExpr *call, MemberExpr *member, llvm::StringRef what);
  WantedCallback wantedCallback(const Type *expected) const;
  const Type *boundFunctionType(IntlKind kind, llvm::StringRef name,
                                const WantedCallback &wanted) const;

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
