#pragma once

#include "compiler/program/library/date/date_format_option_table/date_format_option_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class IndexProofs;
class IntlChecker;
class LiteralTypes;
class TypeRelations;
class GenericChecker;
class ProcessChecker;
class ArgumentChecker;

class DateFormatChecker {
public:
  static bool isLanguageTag(const std::string &tag);
  DateFormatChecker(SemanticState &owner, SemanticParts &all);

  bool checkLocaleArgs(std::vector<Expr *> &args, int kind);
  Expr *checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkIntlCall(CallExpr *call, MemberExpr *member);
  Expr *checkDateFormatMethod(CallExpr *call, MemberExpr *member);
  static const Type *dateFormatPartType();
  static const Type *resolvedDateOptionsType();
  static const Type *dateOptionsType();

private:
  using ResultOptionsObject = Result<const Type *, DateFormatCheckerOptionsObjectError>;

  enum class DateOptionRole {
    Setting,
    Component,
    Style,
  };

  struct DateOptionPlace {
    const Expr *given;
    llvm::SMLoc at;
    bool exempt;
  };

  bool optionKindsFit(const Expr *options, int kind);
  bool optionKindFits(const Expr *options, const Field &field,
                      std::optional<DateFormatOption> option, int kind);
  static const Type *wantedOptionType(DateFormatOption option, const Type *value);
  static DateOptionRole optionRole(DateFormatOption option);
  bool styleFitsMethod(const Expr *options, llvm::StringRef name, int kind);
  bool optionValuesFit(const Expr *options);
  static const Expr *givenProperty(const ObjectLitExpr *literal, llvm::StringRef name);
  bool digitsFit(const Field &field, const DateOptionPlace &place);
  bool optionTextFits(const Field &field, const DateOptionPlace &place);

  static ResultOptionsObject optionsObject(const Type *type, int kind);
  static std::optional<std::u16string> literalText(const Expr *e);
  static std::optional<std::vector<std::string>> textsOf(const Type *type);
  static std::optional<std::vector<double>> numbersOf(const Type *type);
  static const char *methodName(int kind);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  IntlChecker *intlChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
