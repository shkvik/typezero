#include "compiler/library/reflect/reflect_feature/reflect_feature.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/reflect/reflect_call_checker/reflect_call_checker.h"

namespace tz {

Expr *ReflectFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                      const Type *expected) const {
  if (!CallChecker(state).isGlobalName(member->object, "Reflect")) {
    return nullptr;
  }

  return ReflectCallChecker(state).checkReflectCall(call, member, expected);
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
  case Builtin::ReflectMethod:
  case Builtin::ReflectFill:
    return true;
  default:
    return false;
  }
}

llvm::Value *ReflectFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  if (isMetaBuiltin(call->builtin())) {
    return lowering.decoratorLowerer().emitMetaCall(call);
  }

  return lowering.reflectLowerer().emitReflectCall(call);
}

}
