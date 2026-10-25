#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// node:crypto: calls of the functions of the runtime by their description.
class CryptoLowerer {
public:
  explicit CryptoLowerer(CodeGen &codegen);

  // node:crypto (codegen_crypto.cpp): a call of runtime/library/crypto/, and
  // generateKeyPairSync.
  llvm::Value *emitCryptoCall(CallExpr *call);
  llvm::Value *emitCryptoKeyPair(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
