#include "compiler/semantic/library/path/path_feature/path_feature.h"
#include "compiler/semantic/library/path/path_checker/path_checker.h"
#include "compiler/semantic/library/path/path_exports/path_exports.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

bool PathFeature::isPathModule(NodeModule module) {
  return module == NodeModule::Path || module == NodeModule::PathPosix ||
         module == NodeModule::PathWin32;
}

PathFeature::ResultExportFound PathFeature::exportFound(NodeModule module,
                                                        llvm::StringRef exportName,
                                                        llvm::StringRef path) const {
  if (PathFeature::isPathModule(module)) {
    return PathExports::pathExport(exportName, path);
  }

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NOT_MINE,
      .message = "",
  });
}

Expr *PathFeature::checkModuleCall(SemanticState &state, CallExpr *call, NodeModule module,
                                   llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (!PathFeature::isPathModule(module)) {
    return nullptr;
  }

  return PathChecker(state).checkPathCall(call, module, fn, loc);
}

Expr *PathFeature::checkModuleValue(SemanticState &state, NodeModule module,
                                    llvm::StringRef exportName, Expr *expr) const {
  if (!PathFeature::isPathModule(module)) {
    return nullptr;
  }

  return PathChecker(state).checkPathValue(module, exportName, expr);
}

}
