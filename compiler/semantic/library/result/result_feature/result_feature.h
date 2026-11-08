#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class ResultFeature : public Feature {
public:
  llvm::StringRef name() const override { return "result"; }

  Expr *checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkFunctionCall(SemanticState &state, CallExpr *call, NameExpr *name,
                          const Type *expected) const override;
};

}
