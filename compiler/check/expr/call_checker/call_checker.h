#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Calls: which class checks a call of a function, a method or a value.
class CallChecker {
public:
  explicit CallChecker(Checker &checker);

  Expr *checkValueCall(CallExpr *call, const Type *fnType);
  Expr *checkCall(CallExpr *call, const Type *expected);
  // A call to a function known by name; `loc` is where to report the
  // arguments.
  Expr *checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc,
                        const Type *expected = nullptr);
  // Reports spread arguments where the callee takes none (only rest
  // parameters, Math.min, Math.max and push do); checks the arguments loosely
  // then.
  bool rejectSpread(CallExpr *call);
  // Whether `expr` names a built-in global such as `console` that the program
  // does not shadow.
  bool isGlobalName(const Expr *expr, llvm::StringRef name) const;

private:
  Checker &checker_;
};

} // namespace tz
