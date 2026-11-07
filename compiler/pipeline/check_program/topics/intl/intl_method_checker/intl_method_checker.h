#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/intl/intl_method_checker/intl_method_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class IntlMethodChecker {
public:
  explicit IntlMethodChecker(CheckState &owner);

  Expr *checkIntlMethod(CallExpr *call, MemberExpr *member);
  Expr *checkNumberLocale(CallExpr *call, MemberExpr *member);
  Expr *checkStringLocale(CallExpr *call, MemberExpr *member);
  Expr *checkIntlNumber(Expr *arg, llvm::StringRef method);

private:
  static std::optional<std::u16string> literalText(const Expr *e);
  static const Type *relativePartType();
  using ResultClosedText = Result<const Expr *, IntlMethodCheckerClosedArgError>;
  using ResultIntlNumber = Result<const Type *, IntlMethodCheckerIntlNumberError>;

  static ResultIntlNumber intlNumber(const Type *type, llvm::StringRef method);
  static ResultClosedText closedArg(const Expr *arg,
                                    const std::optional<std::vector<std::string>> &texts,
                                    llvm::ArrayRef<llvm::StringLiteral> allowed,
                                    llvm::StringRef what);

  CheckState &state;
};

}
