#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CallbackChecker {
public:
  explicit CallbackChecker(Checker &checker);

  Expr *checkFunctionArg(Expr *arg, const Checker::Signature &expected, unsigned primary,
                         llvm::StringRef method);
  Expr *checkCallbackMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  bool keepsDefined(const Expr *callback) const;

private:
  Checker &checker_;
};

}
