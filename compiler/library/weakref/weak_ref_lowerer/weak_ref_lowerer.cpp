#include "compiler/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

WeakRefLowerer::WeakRefLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *WeakRefLowerer::emitWeakRefCall(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  if (call->builtin == Builtin::WeakRefNew) {
    // The WeakRef borrows the object, which it does not hold.
    Expr *arg = call->args[0];
    llvm::Value *target = codegen_.exprLowerer().emitExpr(arg);
    llvm::Value *weak = codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_weak_new", ptr, {ptr, ptr}),
                                            {codegen_.objectLayouts().layoutDescriptor(call->type, {}), target}, "weak");
    codegen_.refCounts().releaseValue(target, arg->type);
    return weak;
  }
  CodeGen::Operand weak = codegen_.refCounts().emitOperand(cast<MemberExpr>(call->callee)->object, /*canBorrow=*/true);
  llvm::Value *target =
      codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_weak_deref", ptr, {ptr}), {weak.value}, "target");
  codegen_.refCounts().dropOperand(weak);

  return target;
}

} // namespace tz
