#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// node:crypto: calls of the functions of the runtime by their description.
class CryptoLowerer {
public:
  explicit CryptoLowerer(Lowering &lowering);

  // node:crypto (codegen_crypto.cpp): a call of runtime/library/crypto/, and
  // generateKeyPairSync.
  llvm::Value *emitCryptoCall(CallExpr *call);
  llvm::Value *emitCryptoKeyPair(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
