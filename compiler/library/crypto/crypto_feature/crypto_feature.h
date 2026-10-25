#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// node:crypto and Web Crypto (docs/crypto.md).
class CryptoFeature : public Feature {
public:
  llvm::StringRef name() const override { return "crypto"; }

  Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  Expr *checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  Expr *checkClassMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const override;
};

} // namespace tz
