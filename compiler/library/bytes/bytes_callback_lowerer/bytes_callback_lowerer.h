#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The methods of typed arrays that take a callback.
class BytesCallbackLowerer {
public:
  explicit BytesCallbackLowerer(CodeGen &codegen);

  // forEach, map, filter, reduce... of a typed array (see emitCallbackMethod).
  llvm::Value *emitBytesCallbackMethod(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
