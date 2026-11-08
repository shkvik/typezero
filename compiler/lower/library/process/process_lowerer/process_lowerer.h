#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ProcessLowerer {
public:
  explicit ProcessLowerer(Lowering &owner);

  llvm::Value *emitProcessBuiltin(CallExpr *call);
  llvm::FunctionCallee processFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  void emitPanic(llvm::StringRef message);

private:
  Lowering &lowering;
};

}
