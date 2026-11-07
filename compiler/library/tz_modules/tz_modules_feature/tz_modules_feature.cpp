#include "compiler/library/tz_modules/tz_modules_feature/tz_modules_feature.h"
#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"

namespace tz {

void TzModulesFeature::prepare(CheckState &state, SourceFile &file) const {
  LibraryCalls(state).prepareLibraries(file);
}

Expr *TzModulesFeature::checkFunctionCall(CheckState &state, CallExpr *call, NameExpr *name,
                                          const Type *) const {
  return LibraryCalls(state).checkLibCall(call, name);
}

bool TzModulesFeature::lowers(Builtin builtin) const { return isLibBuiltin(builtin); }

llvm::Value *TzModulesFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.libraryCallLowerer().emitLibBuiltin(call);
}

}
