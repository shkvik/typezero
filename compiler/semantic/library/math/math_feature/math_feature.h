#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class MathFeature : public Feature {
public:
  llvm::StringRef name() const override { return "math"; }

  Expr *checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
};

}
