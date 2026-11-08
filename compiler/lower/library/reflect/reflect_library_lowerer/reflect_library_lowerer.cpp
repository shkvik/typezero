#include "compiler/lower/library/reflect/reflect_library_lowerer/reflect_library_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool ReflectLibraryLowerer::lowers(Builtin builtin) const {
  switch (builtin) {
    case Builtin::MetaDefine:
    case Builtin::MetaGet:
    case Builtin::MetaGetOwn:
    case Builtin::MetaHas:
    case Builtin::MetaHasOwn:
    case Builtin::MetaDelete:
    case Builtin::ClassName:
    case Builtin::ReflectGet:
    case Builtin::ReflectApply:
    case Builtin::ReflectConstruct:
    case Builtin::ReflectMethod:
    case Builtin::ReflectFill:
      return true;
    default:
      return false;
  }
}

llvm::Value *ReflectLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  if (BuiltinGroups::isMetaBuiltin(call->builtin())) {
    return lowering.decoratorLowerer().emitMetaCall(call);
  }

  return lowering.reflectLowerer().emitReflectCall(call);
}

}
