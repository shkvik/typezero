#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// The built-in modules written in tz: where they are, and their calls of the compiler
// (the intrinsics __tz_*).
class TzModulesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "tz_modules"; }

  void prepare(Checker &checker, SourceFile &file) const override;
  Expr *checkFunctionCall(Checker &checker, CallExpr *call, NameExpr *name,
                          const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

} // namespace tz
