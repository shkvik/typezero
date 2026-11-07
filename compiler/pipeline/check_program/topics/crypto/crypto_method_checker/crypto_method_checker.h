#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_method_checker/crypto_method_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class CryptoMethodChecker {
public:
  explicit CryptoMethodChecker(CheckState &owner);

  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultIssuerChecked = Result<const Expr *, CryptoMethodCheckerIssuerError>;

  static ResultIssuerChecked issuer(const Expr *other);

  CheckState &state;
};

}
