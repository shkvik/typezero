#include "compiler/lower/library/math/math_library_lowerer/math_library_lowerer.h"
#include "compiler/lower/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool MathLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isMathBuiltin(builtin);
}

llvm::Value *MathLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.mathLowerer().emitMathCall(call);
}

}
