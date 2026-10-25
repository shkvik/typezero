#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// parseInt, parseFloat, isNaN, isFinite, the functions of URIs and the legacy escapes.
class GlobalFunctionChecker {
public:
  explicit GlobalFunctionChecker(Checker &checker);

  // String(x), Number(x), parseInt... (sema_builtins.cpp); null if `name` is
  // not one of them.
  Expr *checkGlobalCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkGlobalFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                            const Type *expected);
  // escape(s) and unescape(s), legacy functions that are errors; the hint
  // names what replaces them, or what the idiom around them means.
  Expr *reportLegacyEscape(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

} // namespace tz
