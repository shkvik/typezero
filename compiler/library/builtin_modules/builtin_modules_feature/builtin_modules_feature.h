#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class BuiltinModulesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "builtin_modules"; }

  Expr *checkAnyCall(Checker &checker, CallExpr *call, const Type *expected) const override;
  Expr *checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  std::optional<std::string> exportProblem(NodeModule module, llvm::StringRef exportName,
                                           llvm::StringRef path) const override;
  Expr *checkModuleCall(Checker &checker, CallExpr *call, NodeModule module, llvm::StringRef fn,
                        llvm::SMLoc loc, const Type *expected) const override;
};

}
