#include "compiler/library/path/path_feature/path_feature.h"
#include "compiler/library/path/path_exports/path_exports.h"
#include "compiler/library/path/path_lowerer/path_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/path/path_checker/path_checker.h"

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

Expr *PathFeature::checkModuleCall(CheckState &state, CallExpr *call, NodeModule module,
                                   llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (!PathFeature::isPathModule(module)) {
    return nullptr;
  }

  return PathChecker(state).checkPathCall(call, module, fn, loc);
}

Expr *PathFeature::checkModuleValue(CheckState &state, NodeModule module,
                                    llvm::StringRef exportName, Expr *expr) const {
  if (!PathFeature::isPathModule(module)) {
    return nullptr;
  }

  return PathChecker(state).checkPathValue(module, exportName, expr);
}

bool PathFeature::lowers(Builtin builtin) const {
  return BuiltinGroups::isPathBuiltin(builtin);
}

llvm::Value *PathFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.pathLowerer().emitPathCall(call);
}

}
