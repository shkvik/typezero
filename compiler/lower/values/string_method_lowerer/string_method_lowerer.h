#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The methods of strings.
class StringMethodLowerer {
public:
  explicit StringMethodLowerer(CodeGen &codegen);

  llvm::Value *emitStringMethod(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
