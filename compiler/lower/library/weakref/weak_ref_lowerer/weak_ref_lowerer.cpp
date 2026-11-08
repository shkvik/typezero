#include "compiler/lower/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;

namespace tz {

WeakRefLowerer::WeakRefLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *WeakRefLowerer::emitWeakRefCall(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  if (call->builtin() == Builtin::WeakRefNew) {
    Expr *arg = call->args[0];
    llvm::Value *target = this->lowering.exprLowerer().emitExpr(arg);
    llvm::Value *weak = this->lowering.builder.CreateCall(
        this->lowering.groupLowerer().groupFunction("tz_weak_new", ptr, {ptr, ptr}),
        {this->lowering.objectLayouts().layoutDescriptor(call->type, {}), target}, "weak");
    this->lowering.refCounts().releaseValue(target, arg->type);
    return weak;
  }
  Lowering::Operand weak =
      this->lowering.refCounts().emitOperand(cast<MemberExpr>(call->callee)->object, true);
  llvm::Value *target = this->lowering.builder.CreateCall(
      this->lowering.groupLowerer().groupFunction("tz_weak_deref", ptr, {ptr}), {weak.value},
      "target");
  this->lowering.refCounts().dropOperand(weak);

  return target;
}

}
