#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// spawnSync, execFileSync and execSync of node:child_process (docs/process.md).
class ChildChecker {
public:
  explicit ChildChecker(Checker &checker);

  // spawnSync, execFileSync and execSync of node:child_process
  // (sema_child.cpp).
  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  // The class ExecSyncError, which execSync and execFileSync throw.
  void declareExecSyncError();

private:
  Checker &checker_;
};

} // namespace tz
