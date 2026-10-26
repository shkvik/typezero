#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class WeakRefFeature : public Feature {
public:
  llvm::StringRef name() const override { return "weakref"; }

  Expr *checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
