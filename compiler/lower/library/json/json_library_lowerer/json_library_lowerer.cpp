#include "compiler/lower/library/json/json_library_lowerer/json_library_lowerer.h"
#include "compiler/lower/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool JsonLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isJsonBuiltin(builtin);
}

llvm::Value *JsonLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.jsonLowerer().emitJsonCall(call);
}

}
