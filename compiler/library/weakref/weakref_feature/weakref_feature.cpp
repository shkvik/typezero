#include "compiler/library/weakref/weakref_feature/weakref_feature.h"
#include "compiler/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/weakref/weak_ref_checker/weak_ref_checker.h"

namespace tz {

Expr *WeakRefFeature::checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                      const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isWeakRef()) {
    return nullptr;
  }

  return WeakRefChecker(state).checkWeakRefMethod(call, member);
}

bool WeakRefFeature::lowers(Builtin builtin) const {
  return builtin == Builtin::WeakRefNew || builtin == Builtin::WeakRefDeref;
}

llvm::Value *WeakRefFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.weakRefLowerer().emitWeakRefCall(call);
}

}
