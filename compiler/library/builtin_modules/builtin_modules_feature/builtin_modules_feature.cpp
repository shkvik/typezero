#include "compiler/library/builtin_modules/builtin_modules_feature/builtin_modules_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/builtin_modules/builtin_module_lowerer/builtin_module_lowerer.h"

namespace tz {

Expr *BuiltinModulesFeature::checkAnyCall(Sema &sema, CallExpr *call, const Type *expected) const {
  return sema.builtinModuleChecker().checkNodeCall(call, expected);
}

Expr *BuiltinModulesFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isRuntimeObject()) {
    return nullptr;
  }

  return sema.runtimeObjectChecker().checkRuntimeMethod(call, member, expected);
}

bool BuiltinModulesFeature::lowers(Builtin builtin) const {
  return isNodeBuiltin(builtin);
}

llvm::Value *BuiltinModulesFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.builtinModuleLowerer().emitNodeBuiltin(call);
}

} // namespace tz
