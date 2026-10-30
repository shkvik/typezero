#include "compiler/library/child_process/child_process_feature/child_process_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/child_process/child_checker/child_checker.h"
#include "compiler/library/child_process/child_error_types/child_error_types.h"
#include "compiler/library/child_process/child_exports/child_exports.h"
#include "compiler/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

void ChildProcessFeature::declare(Checker &checker) const { checker.childErrorTypes().declare(); }

std::optional<std::string> ChildProcessFeature::exportProblem(NodeModule module,
                                                              llvm::StringRef exportName,
                                                              llvm::StringRef path) const {
  if (module != NodeModule::ChildProcess) {
    return std::nullopt;
  }

  return childExportProblem(exportName, path);
}

Expr *ChildProcessFeature::checkModuleCall(Checker &checker, CallExpr *call, NodeModule module,
                                           llvm::StringRef fn, llvm::SMLoc loc,
                                           const Type *) const {
  if (module != NodeModule::ChildProcess) {
    return nullptr;
  }

  return checker.childChecker().checkChildCall(call, fn, loc);
}

bool ChildProcessFeature::lowers(Builtin builtin) const { return isChildBuiltin(builtin); }

llvm::Value *ChildProcessFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.childLowerer().emitChildBuiltin(call);
}

}
