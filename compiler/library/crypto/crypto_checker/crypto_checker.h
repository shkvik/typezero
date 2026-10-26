#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CryptoChecker {
public:
  explicit CryptoChecker(Checker &checker);

  Expr *checkCryptoMember(MemberExpr *member);
  Expr *checkCryptoNew(NewExpr *expr, llvm::StringRef cls);
  Expr *checkX509Parse(CallExpr *call, MemberExpr *member);
  bool isX509Class(Expr *object) const;
  Expr *checkWebCryptoCall(CallExpr *call, MemberExpr *member);
  Expr *checkWebCryptoMember(MemberExpr *member);
  Expr *cryptoConstant(MemberExpr *member);
  Expr *checkCryptoValue(llvm::StringRef name, Expr *expr);
  Expr *checkCryptoModuleMember(MemberExpr *member);
  const Type *cryptoTypeNamed(llvm::StringRef name) const;

private:
  Checker &checker_;
};

}
