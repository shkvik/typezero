#include "compiler/semantic/library/builtin_modules/builtin_modules_feature/builtin_modules_feature.h"
#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/semantic/library/builtin_modules/timers_exports/timers_exports.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *BuiltinModulesFeature::checkAnyCall(SemanticState &state, CallExpr *call,
                                          const Type *expected) const {
  return BuiltinModuleChecker(state).checkNodeCall(call, expected);
}

Expr *BuiltinModulesFeature::checkMethodCall(SemanticState &state, CallExpr *call,
                                             MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isRuntimeObject()) {
    return nullptr;
  }

  return RuntimeObjectChecker(state).checkRuntimeMethod(call, member, expected);
}

BuiltinModulesFeature::ResultExportFound
BuiltinModulesFeature::exportFound(NodeModule module, llvm::StringRef exportName,
                                   llvm::StringRef path) const {
  if (module == NodeModule::TimersPromises) {
    return TimersExports::timersExport(exportName, path);
  }

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NOT_MINE,
      .message = "",
  });
}

Expr *BuiltinModulesFeature::checkModuleCall(SemanticState &state, CallExpr *call,
                                             NodeModule module, llvm::StringRef fn, llvm::SMLoc loc,
                                             const Type *expected) const {
  if (module != NodeModule::TimersPromises) {
    return nullptr;
  }

  return BuiltinModuleChecker(state).checkTimersPromises(call, fn, loc, expected);
}

}
