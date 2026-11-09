#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/crypto_method_checker/crypto_method_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CryptoArgs;
class CallChecker;
class ExprChecker;
class ArgumentChecker;

class CryptoMethodChecker {
public:
  CryptoMethodChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultIssuerChecked = Result<const Expr *, CryptoMethodCheckerIssuerError>;

  static ResultIssuerChecked issuer(const Expr *other);

  SemanticState &state;
  CryptoArgs *cryptoArgs = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
