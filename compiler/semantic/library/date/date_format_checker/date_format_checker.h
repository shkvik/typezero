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
  bool isLanguageTag(const std::string &tag) const;
  DateFormatChecker(SemanticState &owner, SemanticParts &all);

  bool prepareLocaleArgs(std::vector<Expr *> &args, int kind);
  Expr *checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkIntlCall(CallExpr *call, MemberExpr *member);
  Expr *checkDateFormatMethod(CallExpr *call, MemberExpr *member);
  const Type *dateFormatPartType() const;
  const Type *resolvedDateOptionsType() const;
  const Type *dateOptionsType() const;

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
    bool isExempt;
  };

  struct OptionRoles {
    bool hasStyle;
    bool hasComponent;
  };

  bool matchOptionKinds(const Expr *options, int kind);
  bool matchOptionRole(const Expr *options, const Field &field, DateFormatOption option, int kind,
                       OptionRoles &roles);
  bool matchOptionKind(const Expr *options, const Field &field,
                       std::optional<DateFormatOption> option, int kind);
  const Type *wantedOptionType(DateFormatOption option, const Type *value) const;
  DateOptionRole optionRole(DateFormatOption option) const;
  bool matchStyleMethod(const Expr *options, llvm::StringRef name, int kind);
  bool matchOptionValues(const Expr *options);
  const Expr *givenProperty(const ObjectLitExpr *literal, llvm::StringRef name) const;
  bool matchDigits(const Field &field, const DateOptionPlace &place);
  bool isOutsideDigits(double digits) const;
  bool matchOptionText(const Field &field, const DateOptionPlace &place);
  llvm::ArrayRef<llvm::StringLiteral> optionValues(llvm::StringRef name) const;
  std::optional<std::vector<std::string>> optionTexts(const Field &field,
                                                      const DateOptionPlace &place);

  ResultOptionsObject checkOptionsObject(const Type *type, int kind) const;
  std::optional<std::u16string> literalText(const Expr *expr) const;
  std::optional<std::vector<std::string>> textsOf(const Type *type) const;
  std::optional<std::vector<double>> numbersOf(const Type *type) const;
  const char *methodName(int kind) const;

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
