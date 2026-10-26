#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class ReflectFeature : public Feature {
public:
  llvm::StringRef name() const override { return "reflect"; }

  Expr *checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
