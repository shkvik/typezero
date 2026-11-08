#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class BuiltinModulesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "builtin_modules"; }

  Expr *checkAnyCall(SemanticState &state, CallExpr *call, const Type *expected) const override;
  Expr *checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  ResultExportFound exportFound(NodeModule module, llvm::StringRef exportName,
                                llvm::StringRef path) const override;
  Expr *checkModuleCall(SemanticState &state, CallExpr *call, NodeModule module, llvm::StringRef fn,
                        llvm::SMLoc loc, const Type *expected) const override;
};

}
