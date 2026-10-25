#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// process, Date.now() and performance.now() (docs/process.md).
class ProcessFeature : public Feature {
public:
  llvm::StringRef name() const override { return "process"; }

  Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
