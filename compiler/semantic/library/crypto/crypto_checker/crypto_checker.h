#pragma once

#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class CryptoChecker {
public:
  explicit CryptoChecker(SemanticState &owner);

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
  SemanticState &state;
};

}
