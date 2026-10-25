#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// WeakRef.
class WeakRefLowerer {
public:
  explicit WeakRefLowerer(CodeGen &codegen);

  // `new WeakRef(x)` and `w.deref()`.
  llvm::Value *emitWeakRefCall(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
