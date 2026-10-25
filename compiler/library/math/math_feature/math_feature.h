#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// The functions of Math.
class MathFeature : public Feature {
public:
  llvm::StringRef name() const override { return "math"; }

  Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
