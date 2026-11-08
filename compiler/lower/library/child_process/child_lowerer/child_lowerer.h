#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ChildLowerer {
public:
  explicit ChildLowerer(Lowering &owner);

  llvm::Value *emitChildBuiltin(CallExpr *call);
  llvm::Function *spawnMaker(const Type *type);
  llvm::Function *errorMaker(const Type *type);
  llvm::StructType *spawnPartsType();
  llvm::Value *spawnStream(llvm::Value *parts, unsigned stream, const Type *to, bool retain);

private:
  static const Type *bufferType();
  static bool holds(const Type *type, const Type *member);

  Lowering &lowering;
  std::map<const Type *, llvm::Function *> spawnMakers;
};

}
