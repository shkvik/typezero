#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class BytesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "bytes"; }

  Expr *checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  ResultExportFound exportFound(NodeModule module, llvm::StringRef exportName,
                                llvm::StringRef path) const override;
};

}
