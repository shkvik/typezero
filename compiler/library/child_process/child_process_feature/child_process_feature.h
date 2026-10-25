#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// spawnSync, execFileSync and execSync of node:child_process (docs/process.md).
class ChildProcessFeature : public Feature {
public:
  llvm::StringRef name() const override { return "child_process"; }

  void declare(Sema &sema) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
