#include "compiler/library/builtin_modules/builtin_modules_feature/builtin_modules_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/builtin_modules/builtin_module_lowerer/builtin_module_lowerer.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *BuiltinModulesFeature::checkAnyCall(Checker &checker, CallExpr *call,
                                          const Type *expected) const {
  return checker.builtinModuleChecker().checkNodeCall(call, expected);
}

Expr *BuiltinModulesFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                             const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isRuntimeObject()) {
    return nullptr;
  }

  return checker.runtimeObjectChecker().checkRuntimeMethod(call, member, expected);
}

bool BuiltinModulesFeature::lowers(Builtin builtin) const { return isNodeBuiltin(builtin); }

llvm::Value *BuiltinModulesFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.builtinModuleLowerer().emitNodeBuiltin(call);
}

}
