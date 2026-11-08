#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class WeakRefFeature : public Feature {
public:
  llvm::StringRef name() const override { return "weakref"; }

  Expr *checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
};

}
