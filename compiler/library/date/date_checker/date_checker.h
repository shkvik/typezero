#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DateChecker {
public:
  explicit DateChecker(Checker &owner);

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
  void checkDateLiteral(const Expr *arg);
  void checkDateFields(std::vector<Expr *> &args, size_t first = 0, llvm::StringRef setter = "");

private:
  static std::string utf8(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static bool isLeap(int64_t year);
  static int daysInMonth(int64_t year, int64_t month);
  static std::string missingDay(std::optional<int64_t> year, int64_t month, int64_t day);

  Checker &checker;
};

}
