#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class ProcessFeature : public Feature {
public:
  llvm::StringRef name() const override { return "process"; }

  Expr *checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const override;
  ResultExportFound exportFound(NodeModule module, llvm::StringRef exportName,
                                llvm::StringRef path) const override;
};

}
