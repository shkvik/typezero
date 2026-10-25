#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// decimal: its functions and methods (docs/decimal.md).
class DecimalFeature : public Feature {
public:
  llvm::StringRef name() const override { return "decimal"; }

  Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkValueMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
