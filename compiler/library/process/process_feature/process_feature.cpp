#include "compiler/library/process/process_feature/process_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/process/process_exports/process_exports.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *ProcessFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                      const Type *) const {
  return checker.processChecker().checkProcessCall(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ProcessFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"process"};

  return globals;
}

std::optional<std::string> ProcessFeature::exportProblem(NodeModule module,
                                                         llvm::StringRef exportName,
                                                         llvm::StringRef path) const {
  if (module != NodeModule::Process) {
    return std::nullopt;
  }

  return processExportProblem(exportName, path);
}

bool ProcessFeature::lowers(Builtin builtin) const { return isProcessBuiltin(builtin); }

llvm::Value *ProcessFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.processLowerer().emitProcessBuiltin(call);
}

}
