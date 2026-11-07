#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_function_checker/crypto_function_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class CryptoFunctionChecker {
public:
  explicit CryptoFunctionChecker(CheckState &owner);

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

  CheckState &state;
};

}
