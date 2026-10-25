#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The methods of arrays that take a callback, and sort with a comparator.
class CallbackMethodLowerer {
public:
  explicit CallbackMethodLowerer(CodeGen &codegen);

  // Methods of arrays that take a callback (codegen_array.cpp).
  llvm::Value *emitCallbackMethod(CallExpr *call);
  llvm::Function *sortComparator(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
