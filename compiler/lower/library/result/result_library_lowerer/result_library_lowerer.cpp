#include "compiler/lower/library/result/result_library_lowerer/result_library_lowerer.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool ResultLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isResultBuiltin(builtin);
}

llvm::Value *ResultLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.resultLowerer().emitResultCall(call);
}

}
