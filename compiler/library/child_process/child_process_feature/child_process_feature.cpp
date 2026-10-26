#include "compiler/library/child_process/child_process_feature/child_process_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/child_process/child_checker/child_checker.h"
#include "compiler/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

void ChildProcessFeature::declare(Checker &checker) const {
  checker.childChecker().declareExecSyncError();
}

bool ChildProcessFeature::lowers(Builtin builtin) const { return isChildBuiltin(builtin); }

llvm::Value *ChildProcessFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.childLowerer().emitChildBuiltin(call);
}

}
