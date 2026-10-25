#include "compiler/lower/async/async_builtin_lowerer/async_builtin_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/library/result/result_async_lowerer/result_async_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

AsyncBuiltinLowerer::AsyncBuiltinLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *AsyncBuiltinLowerer::emitAsyncBuiltin(CallExpr *call) {
  if (call->builtin >= Builtin::TimersSetTimeout) {
    return codegen_.fsPromisesLowerer().emitAsyncIoBuiltin(call);
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(),
             *f64 = codegen_.builder_.getDoubleTy();
  llvm::Type *voidTy = codegen_.builder_.getVoidTy();
  switch (call->builtin) {
  case Builtin::PromiseNew:
    return codegen_.promiseLowerer().emitPromiseNew(call);
  case Builtin::PromiseThen:
  case Builtin::PromiseCatch:
  case Builtin::PromiseFinally:
    return codegen_.reactionLowerer().emitPromiseReaction(call);
  case Builtin::ResultAsyncFromPromise:
  case Builtin::ResultAsyncFromSafePromise:
  case Builtin::ResultAsyncFromThrowable:
  case Builtin::ResultAsyncMap:
  case Builtin::ResultAsyncMapErr:
  case Builtin::ResultAsyncAndThen:
  case Builtin::ResultAsyncOrElse:
  case Builtin::ResultAsyncMatch:
  case Builtin::ResultAsyncUnwrapOr:
  case Builtin::ResultAsyncAndTee:
  case Builtin::ResultAsyncOrTee:
    return codegen_.resultAsyncLowerer().emitResultAsync(call);
  case Builtin::SetTimeout:
  case Builtin::SetInterval: {
    std::vector<llvm::Value *> args = codegen_.throwLowerer().emitHeldValues(call->args);
    llvm::Value *delay = args.size() > 1 ? args[1] : llvm::ConstantFP::get(f64, 1.0);
    auto *fn =
        cast<llvm::Function>(codegen_.asyncLowerer().asyncFunction("tz_timer_new", ptr, {ptr, ptr, f64, i32}).getCallee());
    fn->addRetAttr(llvm::Attribute::NoAlias);
    return codegen_.builder_.CreateCall(fn,
                               {codegen_.builder_.CreateExtractValue(args[0], 0),
                                codegen_.builder_.CreateExtractValue(args[0], 1), delay,
                                codegen_.builder_.getInt32(call->builtin == Builtin::SetInterval)},
                               "timer");
  }
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick: {
    llvm::Value *callback = codegen_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *info = codegen_.builder_.CreateExtractValue(callback, 0);
    llvm::Value *env = codegen_.builder_.CreateExtractValue(callback, 1);
    if (call->builtin == Builtin::SetImmediate) {
      return codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_immediate_new", ptr, {ptr, ptr}), {info, env},
                                 "immediate");
    }
    codegen_.builder_.CreateCall(
        codegen_.asyncLowerer().asyncFunction(call->builtin == Builtin::NextTick ? "tz_next_tick" : "tz_queue_microtask",
                      voidTy, {ptr, ptr}),
        {info, env});
    return nullptr;
  }
  case Builtin::ClearTimeout:
  case Builtin::ClearImmediate: {
    CodeGen::Operand timer = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *value = timer.value;
    if (call->args[0]->type->isUndefined()) {
      value = llvm::ConstantPointerNull::get(ptr);
    }
    codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction(call->builtin == Builtin::ClearTimeout ? "tz_timer_clear"
                                                                             : "tz_immediate_clear",
                                      voidTy, {ptr}),
                        {value});
    if (!call->args[0]->type->isUndefined()) {
      codegen_.refCounts().dropOperand(timer);
    }
    return nullptr;
  }
  case Builtin::TimerRef:
  case Builtin::TimerUnref:
  case Builtin::TimerRefresh: {
    // The timer itself.
    llvm::Value *timer = codegen_.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
    if (call->builtin == Builtin::TimerRefresh) {
      codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_timer_refresh", voidTy, {ptr}), {timer});
    } else {
      codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_timer_ref", voidTy, {ptr, i32}),
                          {timer, codegen_.builder_.getInt32(call->builtin == Builtin::TimerRef)});
    }
    return timer;
  }
  case Builtin::TimerHasRef: {
    CodeGen::Operand timer = codegen_.refCounts().emitOperand(cast<MemberExpr>(call->callee)->object, /*canBorrow=*/true);
    llvm::Value *has =
        codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_timer_has_ref", i32, {ptr}), {timer.value});
    codegen_.refCounts().dropOperand(timer);
    return codegen_.builder_.CreateIsNotNull(has);
  }
  default:
    return codegen_.promiseLowerer().emitPromiseStatic(call);
  }
}

} // namespace tz
