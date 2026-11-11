#pragma once

#include "compiler/program/library/crypto/crypto_sync_function_table/crypto_sync_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/crypto_function_checker/crypto_function_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/StringMap.h"

namespace tz {

class SemanticParts;
class CryptoArgs;
class CallChecker;
class ExprChecker;
class IndexProofs;
class BytesChecker;
class TypeRelations;
class ConstantFolder;
class KeyPairChecker;
class ArgumentChecker;

class CryptoFunctionChecker {
public:
  CryptoFunctionChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  using ResultFilledTarget = Result<const Type *, CryptoFunctionCheckerFilledTargetError>;
  using ResultCurveIsKnown = Result<llvm::StringRef, CryptoFunctionCheckerCurveIsKnownError>;

  static ResultFilledTarget filledTarget(const Type *type, llvm::StringRef fn,
                                         llvm::StringRef shown);
  static ResultCurveIsKnown curveIsKnown(llvm::StringRef curve);
  static std::optional<std::string> literalOf(const Expr *e);

  struct SyncCall {
    CallExpr *call = nullptr;
    CryptoSyncFunction function = CryptoSyncFunction::CONSTANTS;
    llvm::StringRef fn;
    llvm::StringRef shown;
    llvm::SMLoc loc;
    llvm::StringMap<Expr *> options;
  };

  struct KeyInput {
    Expr *key = nullptr;
    Expr *encoding = nullptr;
    Expr *type = nullptr;
    Expr *passphrase = nullptr;
    int format = 0;
    int has = 0;
  };

  static bool isPresent(const std::vector<Expr *> &args, size_t i);

  Expr *checkSyncCall(CallExpr *call, llvm::StringRef fn, llvm::StringRef shown, llvm::SMLoc loc);
  Expr *asyncOf(Expr *checked);
  Expr *toDouble(Expr *number);
  Expr *checkLoosely(CallExpr *call);
  Expr *checkKnownFunction(SyncCall &sync);
  Expr *reportConstants(CallExpr *call, llvm::SMLoc loc);
  Expr *provenFillOffset(Expr *arg, const Expr *target);
  Expr *provenFillSize(Expr *arg, Expr *at, const Expr *target);
  bool keyOptionsRead(SyncCall &sync, bool priv, KeyInput &input);
  bool keyDetailsRead(llvm::StringMap<Expr *> &options, KeyInput &input);
  Expr *reportUnknownFunction(CallExpr *call, llvm::StringRef shown, llvm::SMLoc loc);
  bool optionsReadAt(SyncCall &sync, size_t i, llvm::ArrayRef<llvm::StringRef> allowed);
  Expr *provenNumber(Expr *arg, int64_t lo, std::optional<int64_t> hi, llvm::StringRef what);
  Expr *checkCreateHash(SyncCall &sync);
  Expr *checkCreateHmac(SyncCall &sync);
  Expr *checkHashOnce(SyncCall &sync);
  Expr *checkRandomBytes(SyncCall &sync);
  Expr *checkRandomUuid(SyncCall &sync);
  Expr *checkRandomInt(SyncCall &sync);
  Expr *checkRandomFill(SyncCall &sync);
  Expr *checkTimingSafeEqual(SyncCall &sync);
  Expr *checkPbkdf2(SyncCall &sync);
  Expr *checkHkdf(SyncCall &sync);
  Expr *checkScrypt(SyncCall &sync);
  bool argonOptionsGiven(SyncCall &sync);
  Expr *checkArgon2(SyncCall &sync);
  Expr *checkCreateCipher(SyncCall &sync);
  Expr *checkNameList(SyncCall &sync);
  Expr *checkCreateSecretKey(SyncCall &sync);
  Expr *checkCreateKey(SyncCall &sync);
  Expr *checkGenerateKeyPair(SyncCall &sync);
  Expr *checkGenerateKey(SyncCall &sync);
  Expr *checkSignOnce(SyncCall &sync);
  Expr *checkCreateSigner(SyncCall &sync);
  Expr *checkCreateEcdh(SyncCall &sync);
  Expr *checkDiffieHellman(SyncCall &sync);
  Expr *checkGeneratePrime(SyncCall &sync);
  Expr *checkCheckPrime(SyncCall &sync);

  SemanticState &state;
  CryptoArgs *cryptoArgs = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  BytesChecker *bytesChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  KeyPairChecker *keyPairChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
