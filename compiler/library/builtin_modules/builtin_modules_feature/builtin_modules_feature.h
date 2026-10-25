#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// The built-in modules of node and tz: the functions they give by their imported names, and
// the objects the runtime makes for them.
class BuiltinModulesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "builtin_modules"; }

  Expr *checkAnyCall(Sema &sema, CallExpr *call, const Type *expected) const override;
  Expr *checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
