#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class UnionLowerer {
public:
  explicit UnionLowerer(Lowering &lowering);

  llvm::Value *emitUnionConversion(llvm::Value *value, const Type *from, const Type *to);
  static bool narrowsUnion(const Type *from, const Type *to);
  llvm::Value *emitUnionEqual(llvm::Value *a, llvm::Value *b, const Type *type);
  llvm::Value *emitUnionTypeOf(llvm::Value *value, const Type *type);
  llvm::Value *emitUnionTruth(llvm::Value *value, const Type *type);
  uint32_t plainTypeId(const Type *type);
  llvm::Value *objectClassId(llvm::Value *object);
  llvm::Value *emitIsOneOf(llvm::Value *id, llvm::ArrayRef<const Type *> types);

private:
  Lowering &lowering_;
  static constexpr uint32_t kPlainIdBase = 0x40000000;
  static constexpr uint32_t kBytesIdBase = 0x3FFFFF00;
  llvm::DenseMap<const Type *, uint32_t> plainIds_;
};

}
