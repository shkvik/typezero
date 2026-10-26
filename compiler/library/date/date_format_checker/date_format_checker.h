#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DateFormatChecker {
public:
  explicit DateFormatChecker(Checker &checker);

  bool checkLocaleArgs(std::vector<Expr *> &args, int kind);
  Expr *checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkIntlCall(CallExpr *call, MemberExpr *member);
  Expr *checkDateFormatMethod(CallExpr *call, MemberExpr *member);
  static const Type *dateFormatPartType();
  static const Type *resolvedDateOptionsType();
  static const Type *dateOptionsType();

private:
  Checker &checker_;
};

}
