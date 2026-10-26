#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ChildLowerer {
public:
  explicit ChildLowerer(Lowering &lowering);

  llvm::Value *emitChildBuiltin(CallExpr *call);
  llvm::Function *spawnMaker(const Type *type);
  llvm::StructType *spawnPartsType();
  llvm::Value *spawnStream(llvm::Value *parts, unsigned stream, const Type *to, bool retain);

private:
  Lowering &lowering_;
  std::map<const Type *, llvm::Function *> spawnMakers_;
};

}
