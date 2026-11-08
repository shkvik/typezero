#include "compiler/lower/library/tz_modules/tz_modules_library_lowerer/tz_modules_library_lowerer.h"
#include "compiler/lower/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool TzModulesLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isLibBuiltin(builtin);
}

llvm::Value *TzModulesLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.libraryCallLowerer().emitLibBuiltin(call);
}

}
