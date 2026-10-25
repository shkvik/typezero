#include "compiler/library/tz_modules/tz_modules_feature/tz_modules_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

void TzModulesFeature::prepare(Checker &checker, SourceFile &file) const {
  checker.libraryCalls().prepareLibraries(file);
}

Expr *TzModulesFeature::checkFunctionCall(Checker &checker, CallExpr *call, NameExpr *name,
                                          const Type * /*expected*/) const {
  return checker.libraryCalls().checkLibCall(call, name);
}

bool TzModulesFeature::lowers(Builtin builtin) const { return isLibBuiltin(builtin); }

llvm::Value *TzModulesFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.libraryCallLowerer().emitLibBuiltin(call);
}

} // namespace tz
