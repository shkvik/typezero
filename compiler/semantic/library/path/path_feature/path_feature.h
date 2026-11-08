#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class PathFeature : public Feature {
public:
  llvm::StringRef name() const override { return "path"; }

  ResultExportFound exportFound(NodeModule module, llvm::StringRef exportName,
                                llvm::StringRef path) const override;
  Expr *checkModuleCall(SemanticState &state, CallExpr *call, NodeModule module, llvm::StringRef fn,
                        llvm::SMLoc loc, const Type *expected) const override;
  Expr *checkModuleValue(SemanticState &state, NodeModule module, llvm::StringRef exportName,
                         Expr *expr) const override;

private:
  static bool isPathModule(NodeModule module);
};

}
