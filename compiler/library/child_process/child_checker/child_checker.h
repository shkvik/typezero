#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// spawnSync, execFileSync and execSync of node:child_process (docs/process.md).
class ChildChecker {
public:
  explicit ChildChecker(Sema &sema);

  // spawnSync, execFileSync and execSync of node:child_process
  // (sema_child.cpp).
  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  // The class ExecSyncError, which execSync and execFileSync throw.
  void declareExecSyncError();

private:
  Sema &sema_;
};

} // namespace tz
