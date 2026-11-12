#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CryptoArgs;
class ExprChecker;
class ClassDeclarer;
class ArgumentChecker;

struct SubtleIntrinsic;

class WebCryptoChecker {
public:
  WebCryptoChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkSubtleIntrinsic(CallExpr *call, NameExpr *name, llvm::StringRef intrinsicName);
  Expr *subtleObject(MemberExpr *member);
  bool isSubtleClass(const Type *type, llvm::StringRef name) const;

private:
  using ResultSubtleAvailable = Result<VarSymbol *, WebCryptoCheckerSubtleAvailableError>;
  using ResultAlgorithmChosen = Result<const Expr *, WebCryptoCheckerLiteralAlgorithmError>;
  using ResultSubtleCallShape = Result<const SubtleIntrinsic *, WebCryptoCheckerSubtleShapeError>;

  ResultSubtleCallShape subtleShape(const CallExpr *call, llvm::StringRef intrinsicName) const;
  ResultAlgorithmChosen literalAlgorithm(const Expr *checked, char kind,
                                         llvm::StringRef intrinsicName) const;
  Expr *checkSubtleArg(Expr *arg, char kind, llvm::StringRef intrinsicName);
  Expr *checkSubtleArgKind(Expr *arg, char kind);
  const Type *subtleResult(char result) const;
  ResultSubtleAvailable subtleAvailable() const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  CryptoArgs *cryptoArgs = nullptr;
  ExprChecker *exprChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
