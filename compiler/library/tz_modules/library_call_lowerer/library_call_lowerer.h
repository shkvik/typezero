#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class LibraryCallLowerer {
public:
  explicit LibraryCallLowerer(Lowering &lowering);

  llvm::Value *emitLibBuiltin(CallExpr *call);
  llvm::Value *nativeSignal(llvm::Value *signal, const Type *type, bool retain = false);
  static bool isAbortSignal(const Type *type);

private:
  Lowering &lowering_;
};

}
