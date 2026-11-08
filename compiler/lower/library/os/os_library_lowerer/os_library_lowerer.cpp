#include "compiler/lower/library/os/os_library_lowerer/os_library_lowerer.h"
#include "compiler/lower/library/os/os_lowerer/os_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool OsLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isOsBuiltin(builtin);
}

llvm::Value *OsLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.osLowerer().emitOsCall(call);
}

}
