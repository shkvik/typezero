#include "compiler/library/child_process/child_process_feature/child_process_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/child_process/child_checker/child_checker.h"
#include "compiler/library/child_process/child_lowerer/child_lowerer.h"

namespace tz {

void ChildProcessFeature::declare(Sema &sema) const {
  sema.childChecker().declareExecSyncError();
}

bool ChildProcessFeature::lowers(Builtin builtin) const {
  return isChildBuiltin(builtin);
}

llvm::Value *ChildProcessFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.childLowerer().emitChildBuiltin(call);
}

} // namespace tz
