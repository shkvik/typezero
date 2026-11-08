#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

struct SubtleIntrinsic;

class WebCryptoChecker {
public:
  explicit WebCryptoChecker(SemanticState &owner);

  Expr *checkSubtleIntrinsic(CallExpr *call, NameExpr *name, llvm::StringRef intrinsicName);
  Expr *subtleObject(MemberExpr *member);
  bool isSubtleClass(const Type *type, llvm::StringRef name) const;

private:
  using ResultSubtleAvailable = Result<VarSymbol *, WebCryptoCheckerSubtleAvailableError>;
  using ResultAlgorithmChosen = Result<const Expr *, WebCryptoCheckerLiteralAlgorithmError>;
  using ResultSubtleCallShape = Result<const SubtleIntrinsic *, WebCryptoCheckerSubtleShapeError>;

  static ResultSubtleCallShape subtleShape(const CallExpr *call, llvm::StringRef intrinsicName);
  static ResultAlgorithmChosen literalAlgorithm(const Expr *checked, char kind,
                                                llvm::StringRef intrinsicName);
  ResultSubtleAvailable subtleAvailable() const;

  SemanticState &state;
};

}
