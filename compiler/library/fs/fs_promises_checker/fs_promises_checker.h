#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The functions of node:fs/promises.
class FsPromisesChecker {
public:
  explicit FsPromisesChecker(Checker &checker);

  // node:fs/promises, node:timers/promises and node:readline
  // (sema_async_io.cpp): the calls of their functions, by name.
  Expr *checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

} // namespace tz
