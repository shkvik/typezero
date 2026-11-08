#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class IntlFeature : public Feature {
public:
  llvm::StringRef name() const override { return "intl"; }

  Expr *checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkValueMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                             const Type *expected) const override;
};

}
