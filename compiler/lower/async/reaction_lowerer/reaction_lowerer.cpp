#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

enum PromiseField : unsigned { kRc, kLayout, kHead, kTail, kError, kState, kFlags };

constexpr uint32_t kFulfilled = 1;

// A reaction to a promise (then, catch, finally): { i64 rc, ptr descriptor,
// reactor { ptr next, ptr run, ptr owner, ptr source, i64 ahead }, ptr
// derived, fn onFulfilled, fn onRejected }.
enum ReactionField : unsigned {
  kReactionRc,
  kReactionDesc,
  kReactorNext,
  kReactorRun,
  kReactorOwner,
  kReactorSource,
  kReactorAhead,
  kReactionDerived,
  kReactionFulfilled,
  kReactionRejected,
};

constexpr uint64_t kReactorOffset = 16;

} // namespace

ReactionLowerer::ReactionLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::StructType *ReactionLowerer::reactionType() {
  if (!reactionTy_) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy();
    reactionTy_ = llvm::StructType::create(lowering_.ctx_,
                                           {lowering_.builder_.getInt64Ty(), ptr, ptr, ptr, ptr,
                                            ptr, lowering_.builder_.getInt64Ty(), ptr,
                                            lowering_.fnTy_, lowering_.fnTy_},
                                           "tz.reaction");
  }

  return reactionTy_;
}

// Releases the promise a reaction gives and its callbacks, then frees it.
llvm::Function *ReactionLowerer::reactionDrop() {
  if (reactionDropFn_) {
    return reactionDropFn_;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  reactionDropFn_ = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.reaction.drop", lowering_.module_.get());
  reactionDropFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  lowering_.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(lowering_.ctx_, "entry", reactionDropFn_));
  llvm::Value *object = reactionDropFn_->getArg(0);
  llvm::StructType *type = reactionType();
  llvm::Value *derived = lowering_.builder_.CreateLoad(
      ptr, lowering_.builder_.CreateStructGEP(type, object, kReactionDerived));
  if (lowering_.memory().hasCycles) {
    // A member of a group (attachReaction): what it holds goes as from one.
    const Type *anyFunction = Type::function({}, 0, false, Type::voidTy());
    lowering_.storeLowerer().releaseIn(object, derived, Type::promise(Type::voidTy()));
    for (unsigned field : {kReactionFulfilled, kReactionRejected}) {
      lowering_.storeLowerer().releaseIn(
          object,
          lowering_.builder_.CreateLoad(lowering_.fnTy_,
                                        lowering_.builder_.CreateStructGEP(type, object, field)),
          anyFunction);
    }
    lowering_.builder_.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_group_obj_free", lowering_.builder_.getVoidTy(),
                                               {lowering_.builder_.getPtrTy()}),
        {object});
  } else {
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {derived});
    for (unsigned field : {kReactionFulfilled, kReactionRejected}) {
      llvm::Value *fn = lowering_.builder_.CreateLoad(
          lowering_.fnTy_, lowering_.builder_.CreateStructGEP(type, object, field));
      lowering_.builder_.CreateCall(lowering_.objReleaseFn_,
                                    {lowering_.builder_.CreateExtractValue(fn, 1)});
    }
    lowering_.builder_.CreateCall(lowering_.rt_.objFree, {object});
  }
  lowering_.builder_.CreateRetVoid();
  lowering_.builder_.restoreIP(point);

  return reactionDropFn_;
}

// void (ptr reactor, ptr source): the code of a reaction, once the promise
// it waits for (`source`, of a value of `value`) settled. It calls the
// callback for how it settled, if there is one, and settles its own promise,
// of a value of `result`, with what that gives, or as `source` did.
llvm::Function *ReactionLowerer::reactionHandler(Builtin kind, const Type *value,
                                                 const Type *result, const Type *onFulfilled,
                                                 const Type *onRejected) {
  llvm::Function *&handler =
      lowering_.reactionHandlers_[{kind, value, result, onFulfilled, onRejected}];
  if (handler) {
    return handler;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  handler = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage,
      kind == Builtin::PromiseFinally ? "tz.promise.finally" : "tz.promise.then",
      lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(handler);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, handler);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", handler));
  llvm::StructType *type = reactionType();
  llvm::Value *object = lowering_.builder_.CreateInBoundsGEP(
      lowering_.builder_.getInt8Ty(), handler->getArg(0),
      llvm::ConstantInt::getSigned(lowering_.builder_.getInt64Ty(), -int64_t(kReactorOffset)),
      "reaction");
  llvm::Value *source = handler->getArg(1);
  llvm::Value *derived = lowering_.builder_.CreateLoad(
      ptr, lowering_.builder_.CreateStructGEP(type, object, kReactionDerived), "derived");
  bool holds = lowering_.promiseLowerer().promiseValueSize(result) != 0;

  // What a callback of type `fnType` gave: its error, a promise to follow, or
  // the value.
  auto settleWith = [&](llvm::Value *given, const Type *fnType) {
    if (lowering_.typeThrows(fnType)) {
      llvm::Value *error =
          lowering_.builder_.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot(), "thrown");
      llvm::BasicBlock *threwBlock = lowering_.functionFrame().newBlock("callback.threw");
      llvm::BasicBlock *gaveBlock = lowering_.functionFrame().newBlock("callback.gave");
      lowering_.builder_.CreateCondBr(
          lowering_.builder_.CreateIsNotNull(error), threwBlock, gaveBlock,
          llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
      lowering_.functionFrame().switchTo(threwBlock);
      lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr),
                                     lowering_.throwLowerer().thrownSlot());
      lowering_.promiseLowerer().rejectPromise(derived, error);
      lowering_.builder_.CreateRetVoid();
      lowering_.functionFrame().switchTo(gaveBlock);
    }
    const Type *ret = fnType->ret();
    if (ret->isPromise()) {
      lowering_.promiseLowerer().followPromise(derived, given);
    } else {
      llvm::Value *converted = nullptr;
      if (holds && !ret->isVoid()) {
        converted = lowering_.exprLowerer().emitConversion(given, ret, result);
      }
      lowering_.promiseLowerer().fulfillPromise(derived, converted, result);
    }
    lowering_.builder_.CreateRetVoid();
  };
  // Calls the callback in `field` of type `fnType` with `arg` (owned; null
  // for none), as many as it takes.
  auto callWith = [&](unsigned field, const Type *fnType, llvm::Value *arg, const Type *argType) {
    llvm::Value *fn = lowering_.builder_.CreateLoad(
        lowering_.fnTy_, lowering_.builder_.CreateStructGEP(type, object, field), "callback");
    std::vector<llvm::Value *> args;
    if (arg && !fnType->params().empty()) {
      args.push_back(arg);
    } else if (arg) {
      lowering_.refCounts().releaseValue(arg, argType);
    }

    return lowering_.closureLowerer().callFunctionValue(fn, fnType, args);
  };

  if (kind == Builtin::PromiseFinally) {
    llvm::Value *given = callWith(kReactionFulfilled, onFulfilled, nullptr, nullptr);
    if (lowering_.typeThrows(onFulfilled)) {
      llvm::Value *error =
          lowering_.builder_.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot(), "thrown");
      llvm::BasicBlock *threwBlock = lowering_.functionFrame().newBlock("callback.threw");
      llvm::BasicBlock *gaveBlock = lowering_.functionFrame().newBlock("callback.gave");
      lowering_.builder_.CreateCondBr(
          lowering_.builder_.CreateIsNotNull(error), threwBlock, gaveBlock,
          llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
      lowering_.functionFrame().switchTo(threwBlock);
      lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr),
                                     lowering_.throwLowerer().thrownSlot());
      lowering_.promiseLowerer().rejectPromise(derived, error);
      lowering_.builder_.CreateRetVoid();
      lowering_.functionFrame().switchTo(gaveBlock);
    }
    llvm::Value *awaited =
        onFulfilled->ret()->isPromise() ? given : llvm::ConstantPointerNull::get(ptr);
    lowering_.builder_.CreateCall(
        lowering_.asyncLowerer().asyncFunction("tz_promise_finally", lowering_.builder_.getVoidTy(),
                                               {ptr, ptr, ptr}),
        {derived, source, awaited});
    lowering_.builder_.CreateRetVoid();
  } else {
    llvm::Value *state = lowering_.builder_.CreateLoad(
        lowering_.builder_.getInt32Ty(), lowering_.promiseLowerer().promiseField(source, kState),
        "state");
    llvm::BasicBlock *fulfilledBlock = lowering_.functionFrame().newBlock("fulfilled");
    llvm::BasicBlock *rejectedBlock = lowering_.functionFrame().newBlock("rejected");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpEQ(state, lowering_.builder_.getInt32(kFulfilled)),
        fulfilledBlock, rejectedBlock);
    lowering_.functionFrame().switchTo(fulfilledBlock);
    llvm::Value *given = nullptr;
    if (lowering_.promiseLowerer().promiseValueSize(value)) {
      given = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(value),
                                            lowering_.promiseLowerer().promiseValueAddress(source),
                                            "value");
      lowering_.refCounts().retainValue(given, value);
    }
    if (onFulfilled) {
      settleWith(callWith(kReactionFulfilled, onFulfilled, given, value), onFulfilled);
    } else {
      lowering_.promiseLowerer().fulfillPromise(
          derived,
          holds && given ? lowering_.exprLowerer().emitConversion(given, value, result) : nullptr,
          result);
      lowering_.builder_.CreateRetVoid();
    }
    lowering_.functionFrame().switchTo(rejectedBlock);
    llvm::Value *error = lowering_.builder_.CreateLoad(
        ptr, lowering_.promiseLowerer().promiseField(source, kError), "error");
    lowering_.groupLowerer().retainObject(error);
    if (onRejected) {
      settleWith(callWith(kReactionRejected, onRejected, error, lowering_.errorClass_->type),
                 onRejected);
    } else {
      lowering_.promiseLowerer().rejectPromise(derived, error);
      lowering_.builder_.CreateRetVoid();
    }
  }
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return handler;
}

llvm::Value *ReactionLowerer::emitPromiseReaction(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *promiseType = member->object->type;
  const Type *value = promiseType->promised(), *result = call->type->promised();
  std::vector<Expr *> exprs{member->object};
  exprs.insert(exprs.end(), call->args.begin(), call->args.end());
  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(exprs);
  llvm::Value *source = values[0];
  // The callbacks, by what they are for; `undefined` is none.
  const Type *types[2] = {nullptr, nullptr};
  llvm::Value *callbacks[2] = {nullptr, nullptr};
  size_t first = call->builtin == Builtin::PromiseCatch ? 1 : 0;
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (!call->args[i]->type->isFunction()) {
      continue;
    }
    types[first + i] = call->args[i]->type;
    callbacks[first + i] = values[i + 1];
  }
  llvm::Value *derived = lowering_.promiseLowerer().newPromise(result);
  attachReaction(source, derived, reactionHandler(call->builtin, value, result, types[0], types[1]),
                 callbacks[0], callbacks[1]);
  lowering_.refCounts().releaseValue(source, promiseType);

  return derived;
}

void ReactionLowerer::attachReaction(llvm::Value *source, llvm::Value *derived,
                                     llvm::Function *handler, llvm::Value *onFulfilled,
                                     llvm::Value *onRejected, bool later) {
  llvm::StructType *type = reactionType();
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(type);
  // In a program with cycles, a member of a group, as what holds it (its
  // promise, see tz_promise_react) and what it holds may be: else a cycle
  // through it could never be freed.
  llvm::Value *object =
      lowering_.memory().hasCycles
          ? lowering_.groupLowerer().allocMember(size, "reaction")
          : lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                          {lowering_.builder_.getInt64(size)}, "reaction");
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  if (!lowering_.memory().hasCycles) {
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                   lowering_.builder_.CreateStructGEP(type, object, kReactionRc));
  }
  // A member's descriptor is a layout, which the report of debug mode reads.
  lowering_.builder_.CreateStore(
      lowering_.memory().hasCycles
          ? lowering_.groupLowerer().memberDescriptor(reactionDrop(), "reaction")
          : lowering_.classDescriptors().dropDescriptor(reactionDrop()),
      lowering_.builder_.CreateStructGEP(type, object, kReactionDesc));
  lowering_.builder_.CreateStore(null,
                                 lowering_.builder_.CreateStructGEP(type, object, kReactorNext));
  lowering_.builder_.CreateStore(handler,
                                 lowering_.builder_.CreateStructGEP(type, object, kReactorRun));
  lowering_.builder_.CreateStore(object,
                                 lowering_.builder_.CreateStructGEP(type, object, kReactorOwner));
  lowering_.builder_.CreateStore(null,
                                 lowering_.builder_.CreateStructGEP(type, object, kReactorSource));
  lowering_.builder_.CreateStore(lowering_.builder_.getInt64(0),
                                 lowering_.builder_.CreateStructGEP(type, object, kReactorAhead));
  lowering_.builder_.CreateCall(lowering_.objRetainFn_, {derived});
  lowering_.builder_.CreateStore(
      derived, lowering_.builder_.CreateStructGEP(type, object, kReactionDerived));
  llvm::Value *callbacks[2] = {onFulfilled, onRejected};
  for (unsigned i = 0; i < 2; ++i) {
    lowering_.builder_.CreateStore(
        callbacks[i] ? callbacks[i] : llvm::Constant::getNullValue(lowering_.fnTy_),
        lowering_.builder_.CreateStructGEP(type, object,
                                           i == 0 ? kReactionFulfilled : kReactionRejected));
  }
  if (lowering_.memory().hasCycles) {
    lowering_.storeLowerer().groupStore(object, derived, Type::promise(Type::voidTy()));
    for (llvm::Value *callback : callbacks) {
      if (callback) {
        lowering_.storeLowerer().groupStore(object, callback,
                                            Type::function({}, 0, false, Type::voidTy()));
      }
    }
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction(later ? "tz_promise_react_later" : "tz_promise_react",
                                             lowering_.builder_.getVoidTy(), {ptr, ptr}),
      {source, lowering_.builder_.CreateStructGEP(type, object, kReactorNext)});
}

} // namespace tz
