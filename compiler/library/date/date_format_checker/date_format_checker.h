#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DateFormatChecker {
public:
  explicit DateFormatChecker(Checker &owner);

  bool checkLocaleArgs(std::vector<Expr *> &args, int kind);
  Expr *checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkIntlCall(CallExpr *call, MemberExpr *member);
  Expr *checkDateFormatMethod(CallExpr *call, MemberExpr *member);
  static const Type *dateFormatPartType();
  static const Type *resolvedDateOptionsType();
  static const Type *dateOptionsType();

private:
  static std::string utf8(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static std::optional<std::vector<std::string>> textsOf(const Type *type);
  static std::optional<std::vector<double>> numbersOf(const Type *type);
  static const char *methodName(int kind);

  Checker &checker;
};

}
