#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// Intl beyond dates, and the methods of strings and numbers that take locales (docs/intl.md).
class IntlFeature : public Feature {
public:
  llvm::StringRef name() const override { return "intl"; }

  Expr *checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  Expr *checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkValueMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                             const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

} // namespace tz
