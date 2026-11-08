#include "compiler/lower/async/async_builtin_lowerer/async_builtin_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;

namespace tz {

AsyncBuiltinLowerer::AsyncBuiltinLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *AsyncBuiltinLowerer::emitAsyncBuiltin(CallExpr *call) {
  if (call->builtin() >= Builtin::TimersSetTimeout) {
    return this->lowering.fsPromisesLowerer().emitAsyncIoBuiltin(call);
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy(), *i32 = this->lowering.builder.getInt32Ty(),
             *f64 = this->lowering.builder.getDoubleTy();
  llvm::Type *voidTy = this->lowering.builder.getVoidTy();
  switch (call->builtin()) {
    case Builtin::PromiseNew:
      return this->lowering.promiseLowerer().emitPromiseNew(call);
    case Builtin::PromiseThen:
      return this->lowering.reactionLowerer().emitPromiseReaction(call);
    case Builtin::SetTimeout:
    case Builtin::SetInterval: {
      std::vector<llvm::Value *> args = this->lowering.exprLowerer().emitExprs(call->args);
      llvm::Value *delay = args.size() > 1 ? args[1] : llvm::ConstantFP::get(f64, 1.0);
      if (args.size() > 2) {
        auto *owned = cast<llvm::Function>(
            this->lowering.asyncLowerer()
                .asyncFunction("tz_timer_new_owned", ptr, {ptr, ptr, f64, i32, i32})
                .getCallee());
        return this->lowering.builder.CreateCall(
            owned,
            {this->lowering.builder.CreateExtractValue(args[0], 0),
             this->lowering.builder.CreateExtractValue(args[0], 1), delay,
             this->lowering.builder.getInt32(call->builtin() == Builtin::SetInterval),
             this->lowering.builder.CreateTrunc(args[2], i32)},
            "timer");
      }
      auto *fn = cast<llvm::Function>(this->lowering.asyncLowerer()
                                          .asyncFunction("tz_timer_new", ptr, {ptr, ptr, f64, i32})
                                          .getCallee());
      fn->addRetAttr(llvm::Attribute::NoAlias);
      return this->lowering.builder.CreateCall(
          fn,
          {this->lowering.builder.CreateExtractValue(args[0], 0),
           this->lowering.builder.CreateExtractValue(args[0], 1), delay,
           this->lowering.builder.getInt32(call->builtin() == Builtin::SetInterval)},
          "timer");
    }
    case Builtin::SetImmediate:
    case Builtin::QueueMicrotask:
    case Builtin::NextTick: {
      llvm::Value *callback = this->lowering.exprLowerer().emitExpr(call->args[0]);
      llvm::Value *info = this->lowering.builder.CreateExtractValue(callback, 0);
      llvm::Value *env = this->lowering.builder.CreateExtractValue(callback, 1);
      if (call->builtin() == Builtin::SetImmediate) {
        return this->lowering.builder.CreateCall(
            this->lowering.asyncLowerer().asyncFunction("tz_immediate_new", ptr, {ptr, ptr}),
            {info, env}, "immediate");
      }
      this->lowering.builder.CreateCall(
          this->lowering.asyncLowerer().asyncFunction(
              call->builtin() == Builtin::NextTick ? "tz_next_tick" : "tz_queue_microtask", voidTy,
              {ptr, ptr}),
          {info, env});
      return nullptr;
    }
    case Builtin::ClearTimeout:
    case Builtin::ClearImmediate: {
      Lowering::Operand timer = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *value = timer.value;
      if (call->args[0]->type->isUndefined()) {
        value = llvm::ConstantPointerNull::get(ptr);
      }
      this->lowering.builder.CreateCall(
          this->lowering.asyncLowerer().asyncFunction(
              call->builtin() == Builtin::ClearTimeout ? "tz_timer_clear" : "tz_immediate_clear",
              voidTy, {ptr}),
          {value});
      if (!call->args[0]->type->isUndefined()) {
        this->lowering.refCounts().dropOperand(timer);
      }
      return nullptr;
    }
    case Builtin::TimerRef:
    case Builtin::TimerUnref:
    case Builtin::TimerRefresh: {
      llvm::Value *timer =
          this->lowering.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
      if (call->builtin() == Builtin::TimerRefresh) {
        this->lowering.builder.CreateCall(
            this->lowering.asyncLowerer().asyncFunction("tz_timer_refresh", voidTy, {ptr}),
            {timer});
      } else {
        this->lowering.builder.CreateCall(
            this->lowering.asyncLowerer().asyncFunction("tz_timer_ref", voidTy, {ptr, i32}),
            {timer, this->lowering.builder.getInt32(call->builtin() == Builtin::TimerRef)});
      }
      return timer;
    }
    case Builtin::TimerHasRef: {
      Lowering::Operand timer =
          this->lowering.refCounts().emitOperand(cast<MemberExpr>(call->callee)->object, true);
      llvm::Value *has = this->lowering.builder.CreateCall(
          this->lowering.asyncLowerer().asyncFunction("tz_timer_has_ref", i32, {ptr}),
          {timer.value});
      this->lowering.refCounts().dropOperand(timer);
      return this->lowering.builder.CreateIsNotNull(has);
    }
    default:
      return this->lowering.promiseLowerer().emitPromiseStatic(call);
  }
}

}
