#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// Date and Intl.DateTimeFormat (docs/date.md).
class DateFeature : public Feature {
public:
  llvm::StringRef name() const override { return "date"; }

  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
