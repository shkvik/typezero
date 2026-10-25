#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The methods of arrays.
class ArrayMethodLowerer {
public:
  explicit ArrayMethodLowerer(CodeGen &codegen);

  llvm::Value *emitArrayMethod(CallExpr *call);
  // A value converted to a string by the runtime, for arrays and optionals.
  llvm::Value *emitValueToString(llvm::Value *value, const Type *type);

private:
  CodeGen &codegen_;
};

} // namespace tz
