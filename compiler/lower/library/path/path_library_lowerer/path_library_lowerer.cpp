#include "compiler/lower/library/path/path_library_lowerer/path_library_lowerer.h"
#include "compiler/lower/library/path/path_lowerer/path_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool PathLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isPathBuiltin(builtin);
}

llvm::Value *PathLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.pathLowerer().emitPathCall(call);
}

}
