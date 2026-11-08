#include "compiler/lower/library/decimal/decimal_library_lowerer/decimal_library_lowerer.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool DecimalLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isDecimalBuiltin(builtin);
}

llvm::Value *DecimalLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.decimalLowerer().emitDecimalCall(call);
}

}
