#include "compiler/semantic/library/os/os_feature/os_feature.h"
#include "compiler/semantic/library/os/os_checker/os_checker.h"
#include "compiler/semantic/library/os/os_exports/os_exports.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

OsFeature::ResultExportFound OsFeature::exportFound(NodeModule module, llvm::StringRef exportName,
                                                    llvm::StringRef path) const {
  if (module == NodeModule::Os) {
    return OsExports::osExport(exportName, path);
  }

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NOT_MINE,
      .message = "",
  });
}

Expr *OsFeature::checkModuleCall(SemanticState &state, CallExpr *call, NodeModule module,
                                 llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (module != NodeModule::Os) {
    return nullptr;
  }

  return OsChecker(state).checkOsCall(call, fn, loc);
}

Expr *OsFeature::checkModuleValue(SemanticState &state, NodeModule module,
                                  llvm::StringRef exportName, Expr *expr) const {
  if (module != NodeModule::Os) {
    return nullptr;
  }

  return OsChecker(state).checkOsValue(exportName, expr);
}

}
