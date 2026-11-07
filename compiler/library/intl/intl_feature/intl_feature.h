#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class IntlFeature : public Feature {
public:
  llvm::StringRef name() const override { return "intl"; }

  Expr *checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkValueMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                             const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
