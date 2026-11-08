#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class RegexFeature : public Feature {
public:
  llvm::StringRef name() const override { return "regex"; }

  Expr *checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *methodReceiver(SemanticState &state, Expr *object) const override;
};

}
