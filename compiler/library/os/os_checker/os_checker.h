#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The functions of node:os.
class OsChecker {
public:
  explicit OsChecker(Sema &sema);

  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Sema &sema_;
};

} // namespace tz
