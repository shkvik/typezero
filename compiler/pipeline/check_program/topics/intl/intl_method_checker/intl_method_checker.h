#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class IntlMethodChecker {
public:
  explicit IntlMethodChecker(CheckState &owner);

  Expr *checkIntlMethod(CallExpr *call, MemberExpr *member);
  Expr *checkNumberLocale(CallExpr *call, MemberExpr *member);
  Expr *checkStringLocale(CallExpr *call, MemberExpr *member);
  Expr *checkIntlNumber(Expr *arg, llvm::StringRef method);

private:
  static std::string utf8(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static const Type *relativePartType();
  static std::string closedArgProblem(const Expr *arg,
                                      const std::optional<std::vector<std::string>> &texts,
                                      llvm::ArrayRef<llvm::StringLiteral> allowed,
                                      llvm::StringRef what);

  CheckState &state;
};

}
