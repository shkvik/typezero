#include "compiler/semantic/library/process/process_feature/process_feature.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/process/process_exports/process_exports.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *ProcessFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
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

}
