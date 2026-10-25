#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// Regular expressions, their matches, and RegExp (docs/regex.md).
class RegexFeature : public Feature {
public:
  llvm::StringRef name() const override { return "regex"; }

  Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  Expr *methodReceiver(Sema &sema, Expr *object) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
