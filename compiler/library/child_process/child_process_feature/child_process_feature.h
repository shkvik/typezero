#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class ChildProcessFeature : public Feature {
public:
  llvm::StringRef name() const override { return "child_process"; }

  void declare(Checker &checker) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
