#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// node:readline.
class ReadlineChecker {
public:
  explicit ReadlineChecker(Sema &sema);

  Expr *checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Sema &sema_;
};

} // namespace tz
