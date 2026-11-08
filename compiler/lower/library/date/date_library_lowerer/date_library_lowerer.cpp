#include "compiler/lower/library/date/date_library_lowerer/date_library_lowerer.h"
#include "compiler/lower/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool DateLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isDateBuiltin(builtin);
}

llvm::Value *DateLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.dateLowerer().emitDateCall(call);
}

}
