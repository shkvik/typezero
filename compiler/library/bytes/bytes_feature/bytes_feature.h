#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class BytesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "bytes"; }

  Expr *checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  ResultExportFound exportFound(NodeModule module, llvm::StringRef exportName,
                                llvm::StringRef path) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
