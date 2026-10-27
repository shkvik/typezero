#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DateChecker {
public:
  explicit DateChecker(Checker &checker);

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
  void checkDateFields(std::vector<Expr *> &args);

private:
  Checker &checker_;
};

}
