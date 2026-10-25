#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// WeakRef (docs/objects.md, section 11).
class WeakRefFeature : public Feature {
public:
  llvm::StringRef name() const override { return "weakref"; }

  Expr *checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
