#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Calls: which class builds a call of a built-in, a method or a function.
class CallLowerer {
public:
  explicit CallLowerer(CodeGen &codegen);

  llvm::Value *emitCall(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
