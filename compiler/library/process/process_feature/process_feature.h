#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class ProcessFeature : public Feature {
public:
  llvm::StringRef name() const override { return "process"; }

  Expr *checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  std::optional<std::string> exportProblem(NodeModule module, llvm::StringRef exportName,
                                           llvm::StringRef path) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
