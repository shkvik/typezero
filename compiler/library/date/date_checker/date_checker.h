#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Date (docs/date.md).
class DateChecker {
public:
  explicit DateChecker(Checker &checker);

  // Dates (sema_date.cpp, docs/date.md). A date is a reference to an object
  // that holds its time value, as in JavaScript. What goes wrong in
  // JavaScript and is visible in the code is an error: a date string
  // JavaScript cannot read or reads as UTC, February 31, month 12, getYear.
  //
  // `new Date(...)`, and `Date()`, which gives the text of now.
  Expr *checkDateNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkDateFunction(CallExpr *call, NameExpr *name);
  // Date.UTC and Date.parse (Date.now is in checkProcessCall).
  Expr *checkDateStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDateStaticMember(MemberExpr *member);
  Expr *checkDateMethod(CallExpr *call, MemberExpr *member);
  Expr *checkDateMember(MemberExpr *member);
  // `a - b` and `a < b` of dates, whose sides are checked: their times. Null
  // if neither side is a date.
  Expr *checkDateOperands(BinaryExpr *bin);
  // getTime() and toString() of a checked date: for `+d`, Number(d), and
  // `${d}` and String(d).
  Expr *dateTime(Expr *date);
  Expr *dateToString(Expr *date);
  // Reports a date string written in the code that JavaScript cannot read,
  // or reads as UTC where a time would make it local; `arg` is not checked.
  void checkDateLiteral(const Expr *arg);
  // The numbers of `new Date(y, m, d...)` and Date.UTC, checked and made
  // doubles, with the errors of those written as constants.
  void checkDateFields(std::vector<Expr *> &args);

private:
  Checker &checker_;
};

} // namespace tz
