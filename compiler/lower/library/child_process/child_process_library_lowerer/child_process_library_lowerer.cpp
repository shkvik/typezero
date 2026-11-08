#include "compiler/lower/library/child_process/child_process_library_lowerer/child_process_library_lowerer.h"
#include "compiler/lower/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool ChildProcessLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isChildBuiltin(builtin);
}

llvm::Value *ChildProcessLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.childLowerer().emitChildBuiltin(call);
}

}
