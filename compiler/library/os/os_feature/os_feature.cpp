#include "compiler/library/os/os_feature/os_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/os/os_checker/os_checker.h"
#include "compiler/library/os/os_exports/os_exports.h"
#include "compiler/library/os/os_lowerer/os_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

std::optional<std::string> OsFeature::exportProblem(NodeModule module, llvm::StringRef exportName,
                                                    llvm::StringRef path) const {
  if (module != NodeModule::Os) {
    return std::nullopt;
  }

  return osExportProblem(exportName, path);
}

Expr *OsFeature::checkModuleCall(Checker &checker, CallExpr *call, NodeModule module,
                                 llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (module != NodeModule::Os) {
    return nullptr;
  }

  return checker.osChecker().checkOsCall(call, fn, loc);
}

Expr *OsFeature::checkModuleValue(Checker &checker, NodeModule module, llvm::StringRef exportName,
                                  Expr *expr) const {
  if (module != NodeModule::Os) {
    return nullptr;
  }

  return checker.osChecker().checkOsValue(exportName, expr);
}

bool OsFeature::lowers(Builtin builtin) const { return isOsBuiltin(builtin); }

llvm::Value *OsFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.osLowerer().emitOsCall(call);
}

}
