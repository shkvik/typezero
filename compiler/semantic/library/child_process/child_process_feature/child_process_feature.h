#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class ChildProcessFeature : public Feature {
public:
  llvm::StringRef name() const override { return "child_process"; }

  void declare(SemanticState &state) const override;
  ResultExportFound exportFound(NodeModule module, llvm::StringRef exportName,
                                llvm::StringRef path) const override;
  Expr *checkModuleCall(SemanticState &state, CallExpr *call, NodeModule module, llvm::StringRef fn,
                        llvm::SMLoc loc, const Type *expected) const override;
};

}
