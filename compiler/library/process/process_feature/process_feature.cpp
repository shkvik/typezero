#include "compiler/library/process/process_feature/process_feature.h"
#include "compiler/library/process/process_exports/process_exports.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker.h"

namespace tz {

Expr *ProcessFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                      const Type *) const {
  return ProcessChecker(state).checkProcessCall(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ProcessFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"process"};

  return globals;
}

ProcessFeature::ResultExportFound ProcessFeature::exportFound(NodeModule module,
                                                              llvm::StringRef exportName,
                                                              llvm::StringRef path) const {
  if (module == NodeModule::Process) {
    return ProcessExports::processExport(exportName, path);
  }

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NOT_MINE,
      .message = "",
  });
}

bool ProcessFeature::lowers(Builtin builtin) const {
  return BuiltinGroups::isProcessBuiltin(builtin);
}

llvm::Value *ProcessFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.processLowerer().emitProcessBuiltin(call);
}

}
