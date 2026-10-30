#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class ChildProcessFeature : public Feature {
public:
  llvm::StringRef name() const override { return "child_process"; }

  void declare(Checker &checker) const override;
  std::optional<std::string> exportProblem(NodeModule module, llvm::StringRef exportName,
                                           llvm::StringRef path) const override;
  Expr *checkModuleCall(Checker &checker, CallExpr *call, NodeModule module, llvm::StringRef fn,
                        llvm::SMLoc loc, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
