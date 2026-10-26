#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class BytesLowerer {
public:
  explicit BytesLowerer(Lowering &lowering);

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
  Lowering &lowering_;
  llvm::StringMap<llvm::Constant *> encodingTables_;
};

}
