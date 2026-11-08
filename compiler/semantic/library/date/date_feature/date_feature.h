#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class DateFeature : public Feature {
public:
  llvm::StringRef name() const override { return "date"; }

  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
};

}
