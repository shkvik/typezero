#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// node:crypto: its types, constants and members, and the global crypto.
class CryptoChecker {
public:
  explicit CryptoChecker(Sema &sema);

  Expr *checkCryptoMember(MemberExpr *member);
  Expr *checkCryptoNew(NewExpr *expr, llvm::StringRef cls);
  Expr *checkWebCryptoCall(CallExpr *call, MemberExpr *member);
  Expr *checkWebCryptoMember(MemberExpr *member);
  Expr *cryptoConstant(MemberExpr *member);
  // The class of node:crypto that `name` names in a type, or null.
  const Type *cryptoTypeNamed(llvm::StringRef name) const;

private:
  Sema &sema_;
};

} // namespace tz
