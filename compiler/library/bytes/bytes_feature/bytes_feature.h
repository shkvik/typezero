#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// ArrayBuffer, typed arrays, DataView, Buffer, TextEncoder and TextDecoder (docs/bytes.md).
class BytesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "bytes"; }

  Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  Expr *checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
