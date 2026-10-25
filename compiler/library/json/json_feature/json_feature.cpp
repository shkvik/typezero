#include "compiler/library/json/json_feature/json_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/json/json_static_checker/json_static_checker.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *JsonFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                   const Type * /*expected*/) const {
  if (checker.callChecker().isGlobalName(member->object, "JSON")) {
    return checker.jsonStaticChecker().checkJsonStatic(call, member);
  }
  if (checker.callChecker().isGlobalName(member->object, "Array") && member->name == "isArray") {
    return checker.jsonChecker().checkArrayIsArray(call, member);
  }

  return nullptr;
}

Expr *JsonFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                   const Type * /*expected*/) const {
  const Type *receiver = member->object->type;
  if (!receiver->nonOptional()->isJson()) {
    return nullptr;
  }

  return checker.jsonChecker().reportJsonMethod(call, member);
}

Expr *JsonFeature::checkFunctionCall(Checker &checker, CallExpr *call, NameExpr *name,
                                     const Type * /*expected*/) const {
  return checker.jsonStaticChecker().checkJsonGlobal(call, name);
}

bool JsonFeature::lowers(Builtin builtin) const { return isJsonBuiltin(builtin); }

llvm::Value *JsonFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.jsonLowerer().emitJsonCall(call);
}

} // namespace tz
