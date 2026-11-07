#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class KeyPairChecker {
public:
  explicit KeyPairChecker(CheckState &owner);

  Expr *checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc, llvm::StringRef shown);

private:
  static std::string utf8Of(const std::u16string &text);
  static std::optional<std::string> literalOf(const Expr *e);

  CheckState &state;
};

}
