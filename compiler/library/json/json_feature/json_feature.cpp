#include "compiler/library/json/json_feature/json_feature.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/json/json_checker/json_checker.h"
#include "compiler/pipeline/check_program/topics/json/json_static_checker/json_static_checker.h"

namespace tz {

Expr *JsonFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                   const Type *) const {
  if (CallChecker(state).isGlobalName(member->object, "JSON")) {
    return JsonStaticChecker(state).checkJsonStatic(call, member);
  }
  if (CallChecker(state).isGlobalName(member->object, "Array") && member->name == "isArray") {
    return JsonChecker(state).checkArrayIsArray(call, member);
  }
  if (CallChecker(state).isGlobalName(member->object, "Json") && member->name == "isJson") {
    return JsonChecker(state).checkJsonIsJson(call, member);
  }

  return nullptr;
}

Expr *JsonFeature::checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                   const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->nonOptional()->isJson()) {
    return nullptr;
  }

  return JsonChecker(state).reportJsonMethod(call, member);
}

Expr *JsonFeature::checkFunctionCall(CheckState &state, CallExpr *call, NameExpr *name,
                                     const Type *) const {
  return JsonStaticChecker(state).checkJsonGlobal(call, name);
}

bool JsonFeature::lowers(Builtin builtin) const { return isJsonBuiltin(builtin); }

llvm::Value *JsonFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.jsonLowerer().emitJsonCall(call);
}

}
