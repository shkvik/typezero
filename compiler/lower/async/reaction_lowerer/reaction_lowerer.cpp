#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
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

ReactionLowerer::ReactionLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::StructType *ReactionLowerer::reactionType() {
  if (!reactionTy_) {
    llvm::Type *ptr = codegen_.builder_.getPtrTy();
    reactionTy_ = llvm::StructType::create(
        codegen_.ctx_,
        {codegen_.builder_.getInt64Ty(), ptr, ptr, ptr, ptr, ptr, codegen_.builder_.getInt64Ty(), ptr, codegen_.fnTy_, codegen_.fnTy_},
        "tz.reaction");
  }

  return reactionTy_;
}

// Releases the promise a reaction gives and its callbacks, then frees it.
llvm::Function *ReactionLowerer::reactionDrop() {
  if (reactionDropFn_) {
    return reactionDropFn_;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  reactionDropFn_ =
      llvm::Function::Create(llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.reaction.drop", codegen_.module_.get());
  reactionDropFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", reactionDropFn_));
  llvm::Value *object = reactionDropFn_->getArg(0);
  llvm::StructType *type = reactionType();
  llvm::Value *derived =
      codegen_.builder_.CreateLoad(ptr, codegen_.builder_.CreateStructGEP(type, object, kReactionDerived));
  if (codegen_.hasCycles_) {
    // A member of a group (attachReaction): what it holds goes as from one.
    const Type *anyFunction = Type::function({}, 0, false, Type::voidTy());
    codegen_.storeLowerer().releaseIn(object, derived, Type::promise(Type::voidTy()));
    for (unsigned field : {kReactionFulfilled, kReactionRejected}) {
      codegen_.storeLowerer().releaseIn(object, codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(type, object, field)),
                anyFunction);
    }
    codegen_.builder_.CreateCall(
        codegen_.groupLowerer().groupFunction("tz_group_obj_free", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}), {object});
  } else {
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {derived});
    for (unsigned field : {kReactionFulfilled, kReactionRejected}) {
      llvm::Value *fn = codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(type, object, field));
      codegen_.builder_.CreateCall(codegen_.objReleaseFn_, {codegen_.builder_.CreateExtractValue(fn, 1)});
    }
    codegen_.builder_.CreateCall(codegen_.rt_.objFree, {object});
  }
  codegen_.builder_.CreateRetVoid();
  codegen_.builder_.restoreIP(point);

  return reactionDropFn_;
}

// void (ptr reactor, ptr source): the code of a reaction, once the promise
// it waits for (`source`, of a value of `value`) settled. It calls the
// callback for how it settled, if there is one, and settles its own promise,
// of a value of `result`, with what that gives, or as `source` did.
llvm::Function *ReactionLowerer::reactionHandler(Builtin kind, const Type *value, const Type *result,
                                         const Type *onFulfilled, const Type *onRejected) {
  llvm::Function *&handler = codegen_.reactionHandlers_[{kind, value, result, onFulfilled, onRejected}];
  if (handler) {
    return handler;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  handler = llvm::Function::Create(
      llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage,
      kind == Builtin::PromiseFinally ? "tz.promise.finally" : "tz.promise.then", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(handler);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = std::exchange(codegen_.function_, handler);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", handler));
  llvm::StructType *type = reactionType();
  llvm::Value *object = codegen_.builder_.CreateInBoundsGEP(
      codegen_.builder_.getInt8Ty(), handler->getArg(0),
      llvm::ConstantInt::getSigned(codegen_.builder_.getInt64Ty(), -int64_t(kReactorOffset)), "reaction");
  llvm::Value *source = handler->getArg(1);
  llvm::Value *derived =
      codegen_.builder_.CreateLoad(ptr, codegen_.builder_.CreateStructGEP(type, object, kReactionDerived), "derived");
  bool holds = codegen_.promiseLowerer().promiseValueSize(result) != 0;

  // What a callback of type `fnType` gave: its error, a promise to follow, or
  // the value.
  auto settleWith = [&](llvm::Value *given, const Type *fnType) {
    if (codegen_.typeThrows(fnType)) {
      llvm::Value *error = codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot(), "thrown");
      llvm::BasicBlock *threwBlock = codegen_.functionFrame().newBlock("callback.threw");
      llvm::BasicBlock *gaveBlock = codegen_.functionFrame().newBlock("callback.gave");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(error), threwBlock, gaveBlock,
                            llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
      codegen_.functionFrame().switchTo(threwBlock);
      codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), codegen_.throwLowerer().thrownSlot());
      codegen_.promiseLowerer().rejectPromise(derived, error);
      codegen_.builder_.CreateRetVoid();
      codegen_.functionFrame().switchTo(gaveBlock);
    }
    const Type *ret = fnType->ret();
    if (ret->isPromise()) {
      codegen_.promiseLowerer().followPromise(derived, given);
    } else {
      llvm::Value *converted = nullptr;
      if (holds && !ret->isVoid()) {
        converted = codegen_.exprLowerer().emitConversion(given, ret, result);
      }
      codegen_.promiseLowerer().fulfillPromise(derived, converted, result);
    }
    codegen_.builder_.CreateRetVoid();
  };
  // Calls the callback in `field` of type `fnType` with `arg` (owned; null
  // for none), as many as it takes.
  auto callWith = [&](unsigned field, const Type *fnType, llvm::Value *arg, const Type *argType) {
    llvm::Value *fn =
        codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(type, object, field), "callback");
    std::vector<llvm::Value *> args;
    if (arg && !fnType->params().empty()) {
      args.push_back(arg);
    } else if (arg) {
      codegen_.refCounts().releaseValue(arg, argType);
    }

    return codegen_.closureLowerer().callFunctionValue(fn, fnType, args);
  };

  if (kind == Builtin::PromiseFinally) {
    llvm::Value *given = callWith(kReactionFulfilled, onFulfilled, nullptr, nullptr);
    if (codegen_.typeThrows(onFulfilled)) {
      llvm::Value *error = codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot(), "thrown");
      llvm::BasicBlock *threwBlock = codegen_.functionFrame().newBlock("callback.threw");
      llvm::BasicBlock *gaveBlock = codegen_.functionFrame().newBlock("callback.gave");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(error), threwBlock, gaveBlock,
                            llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
      codegen_.functionFrame().switchTo(threwBlock);
      codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), codegen_.throwLowerer().thrownSlot());
      codegen_.promiseLowerer().rejectPromise(derived, error);
      codegen_.builder_.CreateRetVoid();
      codegen_.functionFrame().switchTo(gaveBlock);
    }
    llvm::Value *awaited =
        onFulfilled->ret()->isPromise() ? given : llvm::ConstantPointerNull::get(ptr);
    codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_promise_finally", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {derived, source, awaited});
    codegen_.builder_.CreateRetVoid();
  } else {
    llvm::Value *state =
        codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), codegen_.promiseLowerer().promiseField(source, kState), "state");
    llvm::BasicBlock *fulfilledBlock = codegen_.functionFrame().newBlock("fulfilled");
    llvm::BasicBlock *rejectedBlock = codegen_.functionFrame().newBlock("rejected");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(state, codegen_.builder_.getInt32(kFulfilled)),
                          fulfilledBlock, rejectedBlock);
    codegen_.functionFrame().switchTo(fulfilledBlock);
    llvm::Value *given = nullptr;
    if (codegen_.promiseLowerer().promiseValueSize(value)) {
      given = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(value), codegen_.promiseLowerer().promiseValueAddress(source), "value");
      codegen_.refCounts().retainValue(given, value);
    }
    if (onFulfilled) {
      settleWith(callWith(kReactionFulfilled, onFulfilled, given, value), onFulfilled);
    } else {
      codegen_.promiseLowerer().fulfillPromise(derived, holds && given ? codegen_.exprLowerer().emitConversion(given, value, result) : nullptr,
                     result);
      codegen_.builder_.CreateRetVoid();
    }
    codegen_.functionFrame().switchTo(rejectedBlock);
    llvm::Value *error = codegen_.builder_.CreateLoad(ptr, codegen_.promiseLowerer().promiseField(source, kError), "error");
    codegen_.groupLowerer().retainObject(error);
    if (onRejected) {
      settleWith(callWith(kReactionRejected, onRejected, error, codegen_.errorClass_->type), onRejected);
    } else {
      codegen_.promiseLowerer().rejectPromise(derived, error);
      codegen_.builder_.CreateRetVoid();
    }
  }
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);

  return handler;
}

llvm::Value *ReactionLowerer::emitPromiseReaction(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *promiseType = member->object->type;
  const Type *value = promiseType->promised(), *result = call->type->promised();
  std::vector<Expr *> exprs{member->object};
  exprs.insert(exprs.end(), call->args.begin(), call->args.end());
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(exprs);
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
  llvm::Value *derived = codegen_.promiseLowerer().newPromise(result);
  attachReaction(source, derived, reactionHandler(call->builtin, value, result, types[0], types[1]),
                 callbacks[0], callbacks[1]);
  codegen_.refCounts().releaseValue(source, promiseType);

  return derived;
}

void ReactionLowerer::attachReaction(llvm::Value *source, llvm::Value *derived, llvm::Function *handler,
                             llvm::Value *onFulfilled, llvm::Value *onRejected, bool later) {
  llvm::StructType *type = reactionType();
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(type);
  // In a program with cycles, a member of a group, as what holds it (its
  // promise, see tz_promise_react) and what it holds may be: else a cycle
  // through it could never be freed.
  llvm::Value *object =
      codegen_.hasCycles_ ? codegen_.groupLowerer().allocMember(size, "reaction")
                 : codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {codegen_.builder_.getInt64(size)}, "reaction");
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  if (!codegen_.hasCycles_) {
    codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), codegen_.builder_.CreateStructGEP(type, object, kReactionRc));
  }
  // A member's descriptor is a layout, which the report of debug mode reads.
  codegen_.builder_.CreateStore(codegen_.hasCycles_ ? codegen_.groupLowerer().memberDescriptor(reactionDrop(), "reaction")
                                  : codegen_.classDescriptors().dropDescriptor(reactionDrop()),
                       codegen_.builder_.CreateStructGEP(type, object, kReactionDesc));
  codegen_.builder_.CreateStore(null, codegen_.builder_.CreateStructGEP(type, object, kReactorNext));
  codegen_.builder_.CreateStore(handler, codegen_.builder_.CreateStructGEP(type, object, kReactorRun));
  codegen_.builder_.CreateStore(object, codegen_.builder_.CreateStructGEP(type, object, kReactorOwner));
  codegen_.builder_.CreateStore(null, codegen_.builder_.CreateStructGEP(type, object, kReactorSource));
  codegen_.builder_.CreateStore(codegen_.builder_.getInt64(0), codegen_.builder_.CreateStructGEP(type, object, kReactorAhead));
  codegen_.builder_.CreateCall(codegen_.objRetainFn_, {derived});
  codegen_.builder_.CreateStore(derived, codegen_.builder_.CreateStructGEP(type, object, kReactionDerived));
  llvm::Value *callbacks[2] = {onFulfilled, onRejected};
  for (unsigned i = 0; i < 2; ++i) {
    codegen_.builder_.CreateStore(
        callbacks[i] ? callbacks[i] : llvm::Constant::getNullValue(codegen_.fnTy_),
        codegen_.builder_.CreateStructGEP(type, object, i == 0 ? kReactionFulfilled : kReactionRejected));
  }
  if (codegen_.hasCycles_) {
    codegen_.storeLowerer().groupStore(object, derived, Type::promise(Type::voidTy()));
    for (llvm::Value *callback : callbacks) {
      if (callback) {
        codegen_.storeLowerer().groupStore(object, callback, Type::function({}, 0, false, Type::voidTy()));
      }
    }
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction(later ? "tz_promise_react_later" : "tz_promise_react",
                                    codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {source, codegen_.builder_.CreateStructGEP(type, object, kReactorNext)});
}

} // namespace tz
