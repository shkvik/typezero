#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class RegexFeature : public Feature {
public:
  llvm::StringRef name() const override { return "regex"; }

  Expr *checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *methodReceiver(Checker &checker, Expr *object) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
