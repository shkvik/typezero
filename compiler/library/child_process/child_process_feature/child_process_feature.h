#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// spawnSync, execFileSync and execSync of node:child_process (docs/process.md).
class ChildProcessFeature : public Feature {
public:
  llvm::StringRef name() const override { return "child_process"; }

  void declare(Checker &checker) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

} // namespace tz
