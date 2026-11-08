#include "compiler/lower/library/bytes/bytes_library_lowerer/bytes_library_lowerer.h"
#include "compiler/lower/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool BytesLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isBytesBuiltin(builtin);
}

llvm::Value *BytesLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.bytesCallLowerer().emitBytesCall(call);
}

}
