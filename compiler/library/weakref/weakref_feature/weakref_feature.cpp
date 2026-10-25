#include "compiler/library/weakref/weakref_feature/weakref_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *WeakRefFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                      const Type * /*expected*/) const {
  const Type *receiver = member->object->type;
  if (!receiver->isWeakRef()) {
    return nullptr;
  }

  return checker.weakRefChecker().checkWeakRefMethod(call, member);
}

bool WeakRefFeature::lowers(Builtin builtin) const {
  return builtin == Builtin::WeakRefNew || builtin == Builtin::WeakRefDeref;
}

llvm::Value *WeakRefFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.weakRefLowerer().emitWeakRefCall(call);
}

} // namespace tz
