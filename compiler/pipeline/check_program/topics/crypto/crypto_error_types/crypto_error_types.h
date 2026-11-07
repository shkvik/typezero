#pragma once

#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class CryptoErrorTypes {
public:
  explicit CryptoErrorTypes(CheckState &owner);

  void declare();
  const Type *named(llvm::StringRef name);
  const Type *ofCall(CryptoFn fn, llvm::ArrayRef<Expr *> args);
  static bool isCryptoError(const Type *type);

private:
  static bool flag(llvm::ArrayRef<Expr *> args, size_t i);

  CheckState &state;
};

}
