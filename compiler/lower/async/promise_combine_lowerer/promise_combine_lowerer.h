#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class PromiseCombineLowerer {
public:
  explicit PromiseCombineLowerer(Lowering &owner);

  llvm::Value *emitPromiseCombine(CallExpr *call);
  llvm::Function *combineConverter(const Type *from, const Type *to);

private:
  Lowering &lowering;
  std::map<std::pair<const Type *, const Type *>, llvm::Function *> combineConverters;
};

}
