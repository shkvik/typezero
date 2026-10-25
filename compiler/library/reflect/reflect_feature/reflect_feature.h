#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// Reflect and the metadata of decorators (docs/decorators.md, sections 5 and 6).
class ReflectFeature : public Feature {
public:
  llvm::StringRef name() const override { return "reflect"; }

  Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
