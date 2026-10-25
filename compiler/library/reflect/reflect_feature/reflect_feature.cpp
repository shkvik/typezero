#include "compiler/library/reflect/reflect_feature/reflect_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"

namespace tz {

Expr *ReflectFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  if (!sema.callChecker().isGlobalName(member->object, "Reflect")) {
    return nullptr;
  }

  return sema.reflectCallChecker().checkReflectCall(call, member, expected);
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

llvm::Value *ReflectFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  if (isMetaBuiltin(call->builtin)) {
    return codegen.decoratorLowerer().emitMetaCall(call);
  }

  return codegen.reflectLowerer().emitReflectCall(call);
}

} // namespace tz
