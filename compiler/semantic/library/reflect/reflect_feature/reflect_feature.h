#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class ReflectFeature : public Feature {
public:
  llvm::StringRef name() const override { return "reflect"; }

  Expr *checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
};

}
