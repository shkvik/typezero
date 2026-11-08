#include "compiler/lower/library/fs/fs_library_lowerer/fs_library_lowerer.h"
#include "compiler/lower/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool FsLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isFsModuleBuiltin(builtin);
}

llvm::Value *FsLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.fsLowerer().emitFsResult(call);
}

}
