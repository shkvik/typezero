#pragma once

#include "compiler/lower/lowering/lowering.h"
#include <map>

namespace tz {

class CryptoLowerer {
public:
  explicit CryptoLowerer(Lowering &lowering);

  llvm::Value *emitCryptoCall(CallExpr *call);
  llvm::Value *emitCryptoKeyPair(CallExpr *call);

private:
  void useErrors(const Type *errorType);
  llvm::StructType *partsType();
  llvm::Function *variantMaker(const Type *variant);
  llvm::Function *errorMaker(const Type *error);

  Lowering &lowering_;
  std::map<const Type *, llvm::Function *> makers_;
};

}
