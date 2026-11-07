#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class KeyPairChecker {
public:
  explicit KeyPairChecker(Checker &owner);

  Expr *checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc, llvm::StringRef shown);

private:
  static std::string utf8Of(const std::u16string &text);
  static std::optional<std::string> literalOf(const Expr *e);

  Checker &checker;
};

}
