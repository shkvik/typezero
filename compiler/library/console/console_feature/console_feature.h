#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class ConsoleFeature : public Feature {
public:
  llvm::StringRef name() const override { return "console"; }

  Expr *checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
