#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CryptoChecker {
public:
  explicit CryptoChecker(Checker &checker);

  Expr *checkCryptoMember(MemberExpr *member);
  Expr *checkCryptoNew(NewExpr *expr, llvm::StringRef cls);
  Expr *checkWebCryptoCall(CallExpr *call, MemberExpr *member);
  Expr *checkWebCryptoMember(MemberExpr *member);
  Expr *cryptoConstant(MemberExpr *member);
  const Type *cryptoTypeNamed(llvm::StringRef name) const;

private:
  Checker &checker_;
};

}
