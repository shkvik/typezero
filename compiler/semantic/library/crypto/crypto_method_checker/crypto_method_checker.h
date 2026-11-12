#pragma once

#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/crypto/crypto_method_table/crypto_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/crypto_args/crypto_args.h"
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

  struct CryptoMethodCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    CryptoMethod method = CryptoMethod::HASH_UPDATE;
  };

  ResultIssuerChecked checkIssuer(const Expr *other) const;
  bool isPresent(const std::vector<Expr *> &args, size_t index) const;

  Expr *checkLoosely(CallExpr *call);
  Expr *reportNoMethod(CallExpr *call, MemberExpr *member);
  Expr *pointFormat(const CryptoMethodCall &crypto, size_t index);
  Expr *bytesOrText(const CryptoMethodCall &crypto, CryptoFn fn, std::vector<Expr *> fnArgs,
                    size_t index, bool fallible);
  Expr *checkHashUpdate(const CryptoMethodCall &crypto);
  Expr *checkDigest(const CryptoMethodCall &crypto);
  Expr *checkHashCopy(const CryptoMethodCall &crypto);
  Expr *checkSign(const CryptoMethodCall &crypto);
  Expr *checkVerify(const CryptoMethodCall &crypto);
  Expr *checkCipherUpdate(const CryptoMethodCall &crypto);
  Expr *checkCipherFinal(const CryptoMethodCall &crypto);
  Expr *checkSetAad(const CryptoMethodCall &crypto);
  Expr *checkGetAuthTag(const CryptoMethodCall &crypto);
  Expr *checkSetAuthTag(const CryptoMethodCall &crypto);
  Expr *checkSetAutoPadding(const CryptoMethodCall &crypto);
  Expr *checkKeyExport(const CryptoMethodCall &crypto);
  Expr *checkKeyEquals(const CryptoMethodCall &crypto);
  Expr *checkEcdhPublicKey(const CryptoMethodCall &crypto);
  Expr *checkEcdhGetPrivateKey(const CryptoMethodCall &crypto);
  Expr *checkEcdhSetPrivateKey(const CryptoMethodCall &crypto);
  Expr *checkEcdhComputeSecret(const CryptoMethodCall &crypto);
  Expr *checkX509Pem(const CryptoMethodCall &crypto);
  Expr *checkX509Name(const CryptoMethodCall &crypto);
  Expr *checkX509Verify(const CryptoMethodCall &crypto);
  Expr *checkEncodedKeyExport(const CryptoMethodCall &crypto, CryptoOptions &options, bool pem);

  SemanticState &state;
  CryptoArgs *cryptoArgs = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
