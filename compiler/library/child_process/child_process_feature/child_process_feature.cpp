#include "compiler/library/child_process/child_process_feature/child_process_feature.h"
#include "compiler/library/child_process/child_exports/child_exports.h"
#include "compiler/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/child_process/child_checker/child_checker.h"
#include "compiler/pipeline/check_program/topics/child_process/child_error_types/child_error_types.h"

namespace tz {

void ChildProcessFeature::declare(CheckState &state) const {
  ChildErrorTypes(state).declare();
}

ChildProcessFeature::ResultExportFound
ChildProcessFeature::exportFound(NodeModule module, llvm::StringRef exportName,
                                 llvm::StringRef path) const {
  if (module == NodeModule::ChildProcess) {
    return ChildExports::childExport(exportName, path);
  }

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NOT_MINE,
      .message = "",
  });
}

Expr *ChildProcessFeature::checkModuleCall(CheckState &state, CallExpr *call, NodeModule module,
                                           llvm::StringRef fn, llvm::SMLoc loc,
                                           const Type *) const {
  if (module != NodeModule::ChildProcess) {
    return nullptr;
  }

  return ChildChecker(state).checkChildCall(call, fn, loc);
}

bool ChildProcessFeature::lowers(Builtin builtin) const {
  return BuiltinGroups::isChildBuiltin(builtin);
}

llvm::Value *ChildProcessFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.childLowerer().emitChildBuiltin(call);
}

}
