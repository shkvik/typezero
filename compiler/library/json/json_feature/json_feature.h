#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// Json, JSON.parse and JSON.stringify, embedJson and Array.isArray (docs/modules.md,
// section 5).
class JsonFeature : public Feature {
public:
  llvm::StringRef name() const override { return "json"; }

  Expr *checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkFunctionCall(Checker &checker, CallExpr *call, NameExpr *name,
                          const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

} // namespace tz
