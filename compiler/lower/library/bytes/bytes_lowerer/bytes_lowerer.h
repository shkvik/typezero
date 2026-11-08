#pragma once

#include "compiler/lower/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

class BytesLowerer {
public:
  static bool isSignedKind(Bytes kind);
  static llvm::StructType *bytesStruct(llvm::LLVMContext &ctx);

  explicit BytesLowerer(Lowering &owner);

  llvm::Value *emitBytesIndex(IndexExpr *index);
  llvm::Value *emitBytesElementAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitBytesUpdate(UpdateExpr *update);
  llvm::Value *emitBytesMember(MemberExpr *member);
  Lowering::Producer bytesProducer(llvm::Value *slot, const Type *type);
  llvm::Value *bytesData(llvm::Value *bytes);
  llvm::Value *bytesLength(llvm::Value *bytes);
  llvm::Value *loadElement(llvm::Value *data, const Type *type, llvm::Value *index);
  void storeElement(llvm::Value *data, const Type *type, llvm::Value *index, llvm::Value *value);
  llvm::MDNode *bytesTbaa(bool header);
  llvm::Constant *encodingTable(llvm::StringRef name);

private:
  static llvm::Type *storageType(llvm::IRBuilder<> &b, Bytes kind);
  static llvm::Value *toStorage(Lowering &, llvm::IRBuilder<> &b, Bytes kind, llvm::Value *value,
                                llvm::FunctionCallee f16);

  Lowering &lowering;
  llvm::StringMap<llvm::Constant *> encodingTables;
};

}
