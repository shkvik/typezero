#include "compiler/semantic/library/weakref/weakref_feature/weakref_feature.h"
#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *WeakRefFeature::checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                      const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isWeakRef()) {
    return nullptr;
  }

  return WeakRefChecker(state).checkWeakRefMethod(call, member);
}

}
