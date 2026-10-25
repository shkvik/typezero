#include "compiler/library/reflect/reflect_feature/reflect_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *ReflectFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                      const Type *expected) const {
  if (!checker.callChecker().isGlobalName(member->object, "Reflect")) {
    return nullptr;
  }

  return checker.reflectCallChecker().checkReflectCall(call, member, expected);
}

bool ReflectFeature::lowers(Builtin builtin) const {
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
  case Builtin::FromJsonClass:
  case Builtin::ReflectMethod:
    return true;
  default:
    return false;
  }
}

llvm::Value *ReflectFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  if (isMetaBuiltin(call->builtin)) {
    return lowering.decoratorLowerer().emitMetaCall(call);
  }

  return lowering.reflectLowerer().emitReflectCall(call);
}

} // namespace tz
