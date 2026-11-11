#pragma once

#include "compiler/program/library/date/date_method_table/date_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/date/date_checker/date_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <array>

namespace tz {

class SemanticParts;
class Prelude;
class CallChecker;
class ExprChecker;
class TypeRelations;
class ConstantFolder;
class GenericChecker;
class ProcessChecker;
class ArgumentChecker;
class DateFormatChecker;

class DateChecker {
public:
  DateChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkDateNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *timeArg(Expr *arg);
  Expr *checkDateFunction(CallExpr *call, NameExpr *name);
  Expr *checkDateStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDateStaticMember(MemberExpr *member);
  Expr *checkDateMethod(CallExpr *call, MemberExpr *member);
  Expr *checkDateMember(MemberExpr *member);
  Expr *checkDateOperands(BinaryExpr *bin);
  Expr *dateTime(Expr *date);
  Expr *dateToString(Expr *date);
  using ResultDateLiteralChecked = Result<void, DateCheckerLiteralDateError>;

  ResultDateLiteralChecked checkDateLiteral(const Expr *arg);
  void walkDateFields(std::vector<Expr *> &args, size_t first = 0, llvm::StringRef setter = "");

private:
  using ResultLiteralDate = Result<double, DateCheckerLiteralDateError>;

  struct DateFieldValues {
    size_t first = 0;
    llvm::StringRef setter;
    std::array<std::optional<int64_t>, 7> constant;
  };

  void reportField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields, size_t field,
                   const std::string &message);
  void walkYearField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields);
  std::optional<int64_t> validMonth(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields);
  void walkDayField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                    std::optional<int64_t> month);
  void walkDayInMonth(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields, int64_t day,
                      int64_t month);
  void walkTimeFields(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields);

  static ResultLiteralDate literalDate(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static bool isLeap(int64_t year);
  static int daysInMonth(int64_t year, int64_t month);
  static std::string missingDay(std::optional<int64_t> year, int64_t month, int64_t day);
  static std::pair<int64_t, int64_t> carriedDay(int64_t year, int64_t month, int64_t day);
  static std::string unknownMethodMessage(llvm::StringRef name);
  Expr *checkDateMethodForm(CallExpr *call, MemberExpr *member, const DateMethodInfo &method);
  Expr *fieldLiteral(const MemberExpr *member, int value);
  Expr *checkDateSetter(CallExpr *call, MemberExpr *member, const DateMethodInfo &method);
  Expr *checkToLocale(CallExpr *call, MemberExpr *member, const DateMethodInfo &method);

  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  DateFormatChecker *dateFormatChecker = nullptr;
};

}
