#include "compiler/lower/library/process/process_library_lowerer/process_library_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool ProcessLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isProcessBuiltin(builtin);
}

llvm::Value *ProcessLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.processLowerer().emitProcessBuiltin(call);
}

}
