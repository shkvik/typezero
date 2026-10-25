#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// ArrayBuffer, typed arrays, DataView, Buffer, TextEncoder and TextDecoder (docs/bytes.md).
class BytesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "bytes"; }

  Expr *checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

} // namespace tz
