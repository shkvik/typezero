#include "compiler/library/path/path_feature/path_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/path/path_checker/path_checker.h"
#include "compiler/library/path/path_exports/path_exports.h"
#include "compiler/library/path/path_lowerer/path_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

namespace {

bool isPathModule(NodeModule module) {
  return module == NodeModule::Path || module == NodeModule::PathPosix ||
         module == NodeModule::PathWin32;
}

}

std::optional<std::string> PathFeature::exportProblem(NodeModule module, llvm::StringRef exportName,
                                                      llvm::StringRef path) const {
  if (!isPathModule(module)) {
    return std::nullopt;
  }

  return pathExportProblem(exportName, path);
}

Expr *PathFeature::checkModuleCall(Checker &checker, CallExpr *call, NodeModule module,
                                   llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (!isPathModule(module)) {
    return nullptr;
  }

  return checker.pathChecker().checkPathCall(call, module, fn, loc);
}

Expr *PathFeature::checkModuleValue(Checker &checker, NodeModule module, llvm::StringRef exportName,
                                    Expr *expr) const {
  if (!isPathModule(module)) {
    return nullptr;
  }

  return checker.pathChecker().checkPathValue(module, exportName, expr);
}

bool PathFeature::lowers(Builtin builtin) const { return isPathBuiltin(builtin); }

llvm::Value *PathFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.pathLowerer().emitPathCall(call);
}

}
