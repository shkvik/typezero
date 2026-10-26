#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class CryptoLowerer {
public:
  explicit CryptoLowerer(Lowering &lowering);

  llvm::Value *emitCryptoCall(CallExpr *call);
  llvm::Value *emitCryptoKeyPair(CallExpr *call);

private:
  Lowering &lowering_;
};

}
