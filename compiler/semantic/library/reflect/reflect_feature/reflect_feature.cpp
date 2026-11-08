#include "compiler/semantic/library/reflect/reflect_feature/reflect_feature.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *ReflectFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                      const Type *expected) const {
  if (!CallChecker(state).isGlobalName(member->object, "Reflect")) {
    return nullptr;
  }

  return ReflectCallChecker(state).checkReflectCall(call, member, expected);
}

}
