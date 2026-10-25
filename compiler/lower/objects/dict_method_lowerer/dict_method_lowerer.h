#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The methods of Map and Set, new Map, groupBy and Object.fromEntries.
class DictMethodLowerer {
public:
  explicit DictMethodLowerer(CodeGen &codegen);

  llvm::Value *emitDictCall(CallExpr *call);
  llvm::Value *emitDictNew(CallExpr *call);
  llvm::Value *emitGroupBy(CallExpr *call);
  llvm::Value *emitDictObjectCall(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
