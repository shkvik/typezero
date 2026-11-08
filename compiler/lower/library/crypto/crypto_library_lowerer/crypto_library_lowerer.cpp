#include "compiler/lower/library/crypto/crypto_library_lowerer/crypto_library_lowerer.h"
#include "compiler/lower/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool CryptoLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isCryptoBuiltin(builtin);
}

llvm::Value *CryptoLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.cryptoLowerer().emitCryptoCall(call);
}

}
