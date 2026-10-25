#include "compiler/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

WeakRefLowerer::WeakRefLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *WeakRefLowerer::emitWeakRefCall(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  if (call->builtin == Builtin::WeakRefNew) {
    // The WeakRef borrows the object, which it does not hold.
    Expr *arg = call->args[0];
    llvm::Value *target = lowering_.exprLowerer().emitExpr(arg);
    llvm::Value *weak = lowering_.builder_.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_weak_new", ptr, {ptr, ptr}),
        {lowering_.objectLayouts().layoutDescriptor(call->type, {}), target}, "weak");
    lowering_.refCounts().releaseValue(target, arg->type);
    return weak;
  }
  Lowering::Operand weak =
      lowering_.refCounts().emitOperand(cast<MemberExpr>(call->callee)->object, /*canBorrow=*/true);
  llvm::Value *target = lowering_.builder_.CreateCall(
      lowering_.groupLowerer().groupFunction("tz_weak_deref", ptr, {ptr}), {weak.value}, "target");
  lowering_.refCounts().dropOperand(weak);

  return target;
}

} // namespace tz
