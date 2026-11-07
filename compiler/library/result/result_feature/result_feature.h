#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class ResultFeature : public Feature {
public:
  llvm::StringRef name() const override { return "result"; }

  Expr *checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkFunctionCall(CheckState &state, CallExpr *call, NameExpr *name,
                          const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
