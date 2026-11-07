#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CallbackChecker {
public:
  explicit CallbackChecker(Checker &owner);

  Expr *checkFunctionArg(Expr *arg, const Checker::Signature &expected, unsigned primary,
                         llvm::StringRef method);
  Expr *checkCallbackMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  bool keepsDefined(const Expr *callback) const;

private:
  static const char *callbackArgs(llvm::StringRef method);
  static const char *ordinal(size_t i);
  static const char *callbackArgName(llvm::StringRef method, size_t i);

  Checker &checker;
};

}
