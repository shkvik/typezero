#include "compiler/library/weakref/weakref_feature/weakref_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"

namespace tz {

Expr *WeakRefFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isWeakRef()) {
    return nullptr;
  }

  return sema.weakRefChecker().checkWeakRefMethod(call, member);
}

bool WeakRefFeature::lowers(Builtin builtin) const {
  return builtin == Builtin::WeakRefNew || builtin == Builtin::WeakRefDeref;
}

llvm::Value *WeakRefFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.weakRefLowerer().emitWeakRefCall(call);
}

} // namespace tz
