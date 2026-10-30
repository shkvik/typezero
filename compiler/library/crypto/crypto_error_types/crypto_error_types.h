#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"

namespace tz {

class CryptoErrorTypes {
public:
  explicit CryptoErrorTypes(Checker &checker);

  void declare();
  const Type *named(llvm::StringRef name);
  const Type *ofCall(CryptoFn fn, llvm::ArrayRef<Expr *> args);
  static bool isCryptoError(const Type *type);

private:
  Checker &checker_;
};

}
