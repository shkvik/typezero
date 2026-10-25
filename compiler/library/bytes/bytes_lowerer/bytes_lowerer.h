#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Typed arrays, DataView and Buffer: elements read and written in place.
class BytesLowerer {
public:
  explicit BytesLowerer(Lowering &lowering);

  llvm::Value *emitBytesIndex(IndexExpr *index);
  llvm::Value *emitBytesElementAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitBytesUpdate(UpdateExpr *update);
  // `.length` of a typed array, `.byteLength` of a DataView or an ArrayBuffer.
  llvm::Value *emitBytesMember(MemberExpr *member);
  Lowering::Producer bytesProducer(llvm::Value *slot, const Type *type);
  // The data pointer and the length (as i64) of a borrowed object of binary
  // data; their loads are marked apart from those of the elements (see
  // bytesTbaa), so that a loop that writes elements keeps them in registers.
  llvm::Value *bytesData(llvm::Value *bytes);
  llvm::Value *bytesLength(llvm::Value *bytes);
  // Element `index` of a typed array of `type` at `data`, as a value of its
  // element type; and storing such a value, converted as JavaScript does.
  llvm::Value *loadElement(llvm::Value *data, const Type *type, llvm::Value *index);
  void storeElement(llvm::Value *data, const Type *type, llvm::Value *index, llvm::Value *value);
  llvm::MDNode *bytesTbaa(bool header);
  // The constant TzEncodingTable of a single-byte encoding, by name.
  llvm::Constant *encodingTable(llvm::StringRef name);

private:
  Lowering &lowering_;
  llvm::StringMap<llvm::Constant *> encodingTables_;
};

} // namespace tz
