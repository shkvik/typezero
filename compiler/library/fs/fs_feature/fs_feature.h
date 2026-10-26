#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class FsFeature : public Feature {
public:
  llvm::StringRef name() const override { return "fs"; }

  void declare(Checker &checker) const override;
  std::optional<std::string> exportProblem(NodeModule module, llvm::StringRef exportName,
                                           llvm::StringRef path) const override;
  Expr *checkModuleCall(Checker &checker, CallExpr *call, NodeModule module, llvm::StringRef fn,
                        llvm::SMLoc loc, const Type *expected) const override;
  Expr *checkModuleValue(Checker &checker, NodeModule module, llvm::StringRef exportName,
                         Expr *expr) const override;
  Expr *checkModuleMember(Checker &checker, MemberExpr *member) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
