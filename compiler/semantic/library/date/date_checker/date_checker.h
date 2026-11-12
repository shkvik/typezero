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
class BinaryChecker;
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
  Expr *checkDateOperands(BinaryExpr *binary);
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

  ResultLiteralDate checkLiteralDate(const std::u16string &text) const;
  std::optional<std::u16string> literalText(const Expr *expr) const;
  bool isLeap(int64_t year) const;
  int daysInMonth(int64_t year, int64_t month) const;
  std::string missingDay(std::optional<int64_t> year, int64_t month, int64_t day) const;
  std::pair<int64_t, int64_t> carriedDay(int64_t year, int64_t month, int64_t day) const;
  int lastDayOf(std::optional<int64_t> year, int64_t month) const;
  std::string dayOverflowText(llvm::StringRef setter, const std::string &missing, int64_t month,
                              std::optional<int64_t> year, int last) const;
  std::optional<int64_t> nextMonthYear(std::optional<int64_t> year, int64_t month) const;
  Expr *checkDateFromValue(llvm::SMLoc loc, Expr *callee, Expr *given);
  Expr *checkDateFromString(llvm::SMLoc loc, Expr *callee, const Expr *given, Expr *arg);
  Expr *failedDate(llvm::SMLoc loc);
  std::string dateOperatorText(const BinaryExpr *binary, bool isCompare) const;
  std::string unknownMethodMessage(llvm::StringRef name) const;
  Expr *checkDateMethodForm(CallExpr *call, MemberExpr *member, const DateMethodInfo &method);
  Expr *fieldLiteral(const MemberExpr *member, int value);
  Expr *checkDateSetter(CallExpr *call, MemberExpr *member, const DateMethodInfo &method);
  Expr *checkToLocale(CallExpr *call, MemberExpr *member, const DateMethodInfo &method);

  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  DateFormatChecker *dateFormatChecker = nullptr;
};

}
