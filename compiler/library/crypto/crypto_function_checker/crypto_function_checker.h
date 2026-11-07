#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CryptoFunctionChecker {
public:
  explicit CryptoFunctionChecker(Checker &owner);

  Expr *checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  static std::string utf8Of(const std::u16string &text);
  static std::optional<std::string> literalOf(const Expr *e);

  Expr *checkSyncCall(CallExpr *call, llvm::StringRef fn, llvm::StringRef shown, llvm::SMLoc loc);
  Expr *asyncOf(Expr *checked);

  Checker &checker;
};

}
