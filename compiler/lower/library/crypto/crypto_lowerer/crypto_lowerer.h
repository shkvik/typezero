#pragma once

#include "compiler/lower/lowering/lowering.h"
#include <map>

namespace tz {

class CryptoLowerer {
public:
  explicit CryptoLowerer(Lowering &owner);

  llvm::Value *emitCryptoCall(CallExpr *call);
  llvm::Value *emitCryptoKeyPair(CallExpr *call);
  llvm::Value *emitCryptoKeyPairAsync(CallExpr *call);

private:
  static std::string asciiOf(const Type *literal);
  static std::vector<const Type *> literalsOf(const Type *type);

  llvm::Function *pairMaker(const Type *pair);
  void useErrors(const Type *errorType);
  llvm::StructType *partsType();
  llvm::Function *variantMaker(const Type *variant);
  llvm::Function *errorMaker(const Type *error);

  Lowering &lowering;
  std::map<const Type *, llvm::Function *> makers;
};

}
