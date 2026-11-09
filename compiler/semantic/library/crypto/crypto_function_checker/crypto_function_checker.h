#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/crypto_function_checker/crypto_function_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

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

  Expr *checkSyncCall(CallExpr *call, llvm::StringRef fn, llvm::StringRef shown, llvm::SMLoc loc);
  Expr *asyncOf(Expr *checked);

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
