#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Result.combine.
class ResultCombineLowerer {
public:
  explicit ResultCombineLowerer(CodeGen &codegen);

  llvm::Value *emitCombine(CallExpr *call);
  llvm::Value *emitCombineTuple(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
