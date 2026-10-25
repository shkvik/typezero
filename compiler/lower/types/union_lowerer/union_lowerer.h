#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Unions: conversions, equality, typeof and the tests of members.
class UnionLowerer {
public:
  explicit UnionLowerer(Lowering &lowering);

  llvm::Value *emitUnionConversion(llvm::Value *value, const Type *from, const Type *to);
  // Whether `to` holds only members of the union `from` (or of its
  // optional): a conversion that takes a part of the value, or all of it.
  static bool narrowsUnion(const Type *from, const Type *to);
  llvm::Value *emitUnionEqual(llvm::Value *a, llvm::Value *b, const Type *type);
  llvm::Value *emitUnionTypeOf(llvm::Value *value, const Type *type);
  llvm::Value *emitUnionTruth(llvm::Value *value, const Type *type);
  uint32_t plainTypeId(const Type *type);
  llvm::Value *objectClassId(llvm::Value *object);
  // Whether the class number `id` is that of an object of one of `types`.
  llvm::Value *emitIsOneOf(llvm::Value *id, llvm::ArrayRef<const Type *> types);

private:
  Lowering &lowering_;
  // Objects of a union are told apart by the class number in their
  // descriptor; an object type that is no class has a number of its own,
  // from kPlainIdBase on, above those of every class.
  static constexpr uint32_t kPlainIdBase = 0x40000000;
  // Those of the objects of binary data, by their kind (TZ_BYTES_CLASS in
  // runtime/core/tzrt/tzrt.h).
  static constexpr uint32_t kBytesIdBase = 0x3FFFFF00;
  llvm::DenseMap<const Type *, uint32_t> plainIds_;
};

} // namespace tz
