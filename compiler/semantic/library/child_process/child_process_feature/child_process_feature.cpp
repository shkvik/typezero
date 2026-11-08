#include "compiler/semantic/library/child_process/child_process_feature/child_process_feature.h"
#include "compiler/semantic/library/child_process/child_checker/child_checker.h"
#include "compiler/semantic/library/child_process/child_error_types/child_error_types.h"
#include "compiler/semantic/library/child_process/child_exports/child_exports.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

void ChildProcessFeature::declare(SemanticState &state) const {
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

Expr *ChildProcessFeature::checkModuleCall(SemanticState &state, CallExpr *call, NodeModule module,
                                           llvm::StringRef fn, llvm::SMLoc loc,
                                           const Type *) const {
  if (module != NodeModule::ChildProcess) {
    return nullptr;
  }

  return ChildChecker(state).checkChildCall(call, fn, loc);
}

}
