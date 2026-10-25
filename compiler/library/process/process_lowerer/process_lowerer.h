#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// process and the clock.
class ProcessLowerer {
public:
  explicit ProcessLowerer(CodeGen &codegen);

  // process, Date.now() and performance.now() (codegen_process.cpp).
  llvm::Value *emitProcessBuiltin(CallExpr *call);
  // A function of the runtime for those and console.error, declared when
  // first called.
  llvm::FunctionCallee processFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);

private:
  CodeGen &codegen_;
};

} // namespace tz
