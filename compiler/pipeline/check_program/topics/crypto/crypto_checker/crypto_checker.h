#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_checker/crypto_checker_errors.h"

namespace tz {

class CryptoChecker {
public:
  explicit CryptoChecker(CheckState &owner);

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
  CheckState &state;
};

}
