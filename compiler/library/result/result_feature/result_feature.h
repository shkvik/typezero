#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// Result and ResultAsync of neverthrow, built in (docs/errors.md).
class ResultFeature : public Feature {
public:
  llvm::StringRef name() const override { return "result"; }

  Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  Expr *checkFunctionCall(Sema &sema, CallExpr *call, NameExpr *name, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
