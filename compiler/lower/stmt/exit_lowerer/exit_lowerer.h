#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ExitLowerer {
public:
  explicit ExitLowerer(Lowering &owner);

  void emitReturnOf(llvm::Value *value);
  void emitJumpTo(size_t target, bool isBreak);

private:
  static const Type *returnedType(const FunctionDecl *fn, const Type *returnType);
  Lowering &lowering;
};

}
