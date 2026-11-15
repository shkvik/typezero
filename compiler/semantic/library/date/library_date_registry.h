#pragma once

#include "compiler/program/library/date/date_method_table/date_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/date/library_date_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tz {

class LibraryDateRegistry {
public:
  explicit LibraryDateRegistry(SemanticState &owner);

  void dateToTime();
  void dateToString();
  void timeArgSlot(const TimeArgSlot &task);
  void checkDateNew(const CheckDateNew &task);
  void finishTimeArg(const FinishTimeArg &task);
  void afterDateValue(const AfterDateValue &task);
  void finishDateTime(const FinishDateTime &task);
  void checkDateMember(const CheckDateMember &task);
  void checkDateStatic(const CheckDateStatic &task);
  void checkDateMethod(const CheckDateMethod &task);
  void finishDateParse(const FinishDateParse &task);
  void afterLocaleArgs(const AfterLocaleArgs &task);
  void afterDateFields(const AfterDateFields &task);
  void finishDateFields(const FinishDateFields &task);
  void dateSetterFields(const DateSetterFields &task);
  void finishDateSetter(const FinishDateSetter &task);
  void checkDateFunction(const CheckDateFunction &task);
  void checkDateOperands(const CheckDateOperands &task);
  void checkDateStaticMember(const CheckDateStaticMember &task);

private:
  struct DateFieldValues {
    size_t first = 0;
    llvm::StringRef setter;
    std::array<std::optional<int64_t>, 7> constant;
  };

  using ResultLiteralDate = Result<double, LibraryDateRegistryLiteralDateError>;
  using ResultDateLiteralChecked = Result<void, LibraryDateRegistryLiteralDateError>;

  void reportDateStaticMember(MemberExpr *member);
  void startDateNew(llvm::SMLoc loc, std::vector<Expr *> *args);
  void walkYearField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields);
  void walkTimeFields(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields);
  void pushDateFields(std::vector<Expr *> *args, size_t first, llvm::StringRef setter);
  void checkDateMethodForm(CallExpr *call, MemberExpr *member, const DateMethodInfo &method);
  void walkDayField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                    std::optional<int64_t> month);
  void walkDayInMonth(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields, int64_t day,
                      int64_t month);
  void reportField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields, size_t field,
                   const std::string &message);


  bool isLeap(int64_t year) const;


  Expr *dateTime(Expr *date);
  Expr *dateText(Expr *date);
  Expr *failedDate(llvm::SMLoc loc);
  Expr *fieldLiteral(const MemberExpr *member, int value);
  Expr *dateCall(llvm::SMLoc loc, Builtin builtin, std::vector<Expr *> args);


  int daysInMonth(int64_t year, int64_t month) const;
  int lastDayOf(std::optional<int64_t> year, int64_t month) const;


  std::string unknownMethodMessage(llvm::StringRef name) const;
  std::string dateOperatorText(const BinaryExpr *binary, bool isCompare) const;
  std::string missingDay(std::optional<int64_t> year, int64_t month, int64_t day) const;
  std::string dayOverflowText(llvm::StringRef setter, const std::string &missing, int64_t month,
                              std::optional<int64_t> year, int last) const;


  std::optional<int64_t> nextMonthYear(std::optional<int64_t> year, int64_t month) const;
  std::optional<int64_t> validMonth(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields);


  ResultDateLiteralChecked checkDateLiteral(const Expr *arg);
  ResultLiteralDate checkLiteralDate(const std::u16string &text) const;
  std::optional<std::u16string> literalText(const Expr *expr) const;
  std::pair<int64_t, int64_t> carriedDay(int64_t year, int64_t month, int64_t day) const;

  SemanticState &state;
};

}
