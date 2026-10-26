#include "compiler/library/builtin_modules/builtin_modules_feature/builtin_modules_feature.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/builtin_modules/timers_exports/timers_exports.h"

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

std::optional<std::string> BuiltinModulesFeature::exportProblem(NodeModule module,
                                                                llvm::StringRef exportName,
                                                                llvm::StringRef path) const {
  if (module != NodeModule::TimersPromises) {
    return std::nullopt;
  }

  return timersExportProblem(exportName, path);
}

Expr *BuiltinModulesFeature::checkModuleCall(Checker &checker, CallExpr *call, NodeModule module,
                                             llvm::StringRef fn, llvm::SMLoc loc,
                                             const Type *expected) const {
  if (module != NodeModule::TimersPromises) {
    return nullptr;
  }

  return checker.timerChecker().checkTimersPromisesCall(call, fn, loc, expected);
}

}
