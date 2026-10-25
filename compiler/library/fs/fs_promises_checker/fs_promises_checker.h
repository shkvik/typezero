#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The functions of node:fs/promises.
class FsPromisesChecker {
public:
  explicit FsPromisesChecker(Sema &sema);

  // node:fs/promises, node:timers/promises and node:readline
  // (sema_async_io.cpp): the calls of their functions, by name.
  Expr *checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Sema &sema_;
};

} // namespace tz
