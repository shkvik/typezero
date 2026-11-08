#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/crypto_method_checker/crypto_method_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class CryptoMethodChecker {
public:
  explicit CryptoMethodChecker(SemanticState &owner);

  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultIssuerChecked = Result<const Expr *, CryptoMethodCheckerIssuerError>;

  static ResultIssuerChecked issuer(const Expr *other);

  SemanticState &state;
};

}
