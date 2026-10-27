#include "compiler/lower/async/async_builtin_lowerer/async_builtin_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

AsyncBuiltinLowerer::AsyncBuiltinLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *AsyncBuiltinLowerer::emitAsyncBuiltin(CallExpr *call) {
  if (call->builtin >= Builtin::TimersSetTimeout) {
    return lowering_.fsPromisesLowerer().emitAsyncIoBuiltin(call);
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *f64 = lowering_.builder_.getDoubleTy();
  llvm::Type *voidTy = lowering_.builder_.getVoidTy();
  switch (call->builtin) {
  case Builtin::PromiseNew:
    return lowering_.promiseLowerer().emitPromiseNew(call);
  case Builtin::PromiseThen:
    return lowering_.reactionLowerer().emitPromiseReaction(call);
  case Builtin::SetTimeout:
  case Builtin::SetInterval: {
    std::vector<llvm::Value *> args = lowering_.exprLowerer().emitExprs(call->args);
    llvm::Value *delay = args.size() > 1 ? args[1] : llvm::ConstantFP::get(f64, 1.0);
    auto *fn = cast<llvm::Function>(lowering_.asyncLowerer()
                                        .asyncFunction("tz_timer_new", ptr, {ptr, ptr, f64, i32})
                                        .getCallee());
    fn->addRetAttr(llvm::Attribute::NoAlias);
    return lowering_.builder_.CreateCall(
        fn,
        {lowering_.builder_.CreateExtractValue(args[0], 0),
         lowering_.builder_.CreateExtractValue(args[0], 1), delay,
         lowering_.builder_.getInt32(call->builtin == Builtin::SetInterval)},
        "timer");
  }
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick: {
    llvm::Value *callback = lowering_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *info = lowering_.builder_.CreateExtractValue(callback, 0);
    llvm::Value *env = lowering_.builder_.CreateExtractValue(callback, 1);
    if (call->builtin == Builtin::SetImmediate) {
      return lowering_.builder_.CreateCall(
          lowering_.asyncLowerer().asyncFunction("tz_immediate_new", ptr, {ptr, ptr}), {info, env},
          "immediate");
    }
    lowering_.builder_.CreateCall(
        lowering_.asyncLowerer().asyncFunction(
            call->builtin == Builtin::NextTick ? "tz_next_tick" : "tz_queue_microtask", voidTy,
            {ptr, ptr}),
        {info, env});
    return nullptr;
  }
  case Builtin::ClearTimeout:
  case Builtin::ClearImmediate: {
    Lowering::Operand timer = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *value = timer.value;
    if (call->args[0]->type->isUndefined()) {
      value = llvm::ConstantPointerNull::get(ptr);
    }
    lowering_.builder_.CreateCall(
        lowering_.asyncLowerer().asyncFunction(
            call->builtin == Builtin::ClearTimeout ? "tz_timer_clear" : "tz_immediate_clear",
            voidTy, {ptr}),
        {value});
    if (!call->args[0]->type->isUndefined()) {
      lowering_.refCounts().dropOperand(timer);
    }
    return nullptr;
  }
  case Builtin::TimerRef:
  case Builtin::TimerUnref:
  case Builtin::TimerRefresh: {
    llvm::Value *timer = lowering_.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
    if (call->builtin == Builtin::TimerRefresh) {
      lowering_.builder_.CreateCall(
          lowering_.asyncLowerer().asyncFunction("tz_timer_refresh", voidTy, {ptr}), {timer});
    } else {
      lowering_.builder_.CreateCall(
          lowering_.asyncLowerer().asyncFunction("tz_timer_ref", voidTy, {ptr, i32}),
          {timer, lowering_.builder_.getInt32(call->builtin == Builtin::TimerRef)});
    }
    return timer;
  }
  case Builtin::TimerHasRef: {
    Lowering::Operand timer =
        lowering_.refCounts().emitOperand(cast<MemberExpr>(call->callee)->object, true);
    llvm::Value *has = lowering_.builder_.CreateCall(
        lowering_.asyncLowerer().asyncFunction("tz_timer_has_ref", i32, {ptr}), {timer.value});
    lowering_.refCounts().dropOperand(timer);
    return lowering_.builder_.CreateIsNotNull(has);
  }
  default:
    return lowering_.promiseLowerer().emitPromiseStatic(call);
  }
}

}
