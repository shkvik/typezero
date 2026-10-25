#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// generateKeyPair and generateKeyPairSync.
class KeyPairChecker {
public:
  explicit KeyPairChecker(Sema &sema);

  Expr *checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc);

private:
  Sema &sema_;
};

} // namespace tz
