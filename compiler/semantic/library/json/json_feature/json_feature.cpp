#include "compiler/semantic/library/json/json_feature/json_feature.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/json/json_static_checker/json_static_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *JsonFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
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

Expr *JsonFeature::checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                   const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->nonOptional()->isJson()) {
    return nullptr;
  }

  return JsonChecker(state).reportJsonMethod(call, member);
}

Expr *JsonFeature::checkFunctionCall(SemanticState &state, CallExpr *call, NameExpr *name,
                                     const Type *) const {
  return JsonStaticChecker(state).checkJsonGlobal(call, name);
}

}
