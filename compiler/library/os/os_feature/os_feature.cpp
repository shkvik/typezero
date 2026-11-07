#include "compiler/library/os/os_feature/os_feature.h"
#include "compiler/library/os/os_exports/os_exports.h"
#include "compiler/library/os/os_lowerer/os_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/os/os_checker/os_checker.h"

namespace tz {

std::optional<std::string> OsFeature::exportProblem(NodeModule module, llvm::StringRef exportName,
                                                    llvm::StringRef path) const {
  if (module != NodeModule::Os) {
    return std::nullopt;
  }

  return osExportProblem(exportName, path);
}

Expr *OsFeature::checkModuleCall(CheckState &state, CallExpr *call, NodeModule module,
                                 llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (module != NodeModule::Os) {
    return nullptr;
  }

  return OsChecker(state).checkOsCall(call, fn, loc);
}

Expr *OsFeature::checkModuleValue(CheckState &state, NodeModule module, llvm::StringRef exportName,
                                  Expr *expr) const {
  if (module != NodeModule::Os) {
    return nullptr;
  }

  return OsChecker(state).checkOsValue(exportName, expr);
}

bool OsFeature::lowers(Builtin builtin) const { return isOsBuiltin(builtin); }

llvm::Value *OsFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.osLowerer().emitOsCall(call);
}

}
