#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
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

using llvm::cast;

namespace tz {

namespace {

enum ReactionField : unsigned {
  kReactionRc,
  kReactionDesc,
  kReactorNext,
  kReactorRun,
  kReactorOwner,
  kReactorSource,
  kReactorAhead,
  kReactorContext,
  kReactionDerived,
  kReactionCallback,
};

constexpr uint64_t kReactorOffset = 16;

}

ReactionLowerer::ReactionLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::StructType *ReactionLowerer::reactionType() {
  if (!this->reactionTy) {
    llvm::Type *ptr = this->lowering.builder_.getPtrTy();
    this->reactionTy = llvm::StructType::create(
        this->lowering.ctx_,
        {this->lowering.builder_.getInt64Ty(), ptr, ptr, ptr, ptr, ptr,
         this->lowering.builder_.getInt64Ty(), ptr, ptr, this->lowering.fnTy_},
        "tz.reaction");
  }

  return this->reactionTy;
}

llvm::Function *ReactionLowerer::reactionDrop() {
  if (this->reactionDropFn) {
    return this->reactionDropFn;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->reactionDropFn = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder_.getVoidTy(), {ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.reaction.drop", this->lowering.module_.get());
  this->reactionDropFn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", this->reactionDropFn));
  llvm::Value *object = this->reactionDropFn->getArg(0);
  llvm::StructType *type = this->reactionType();
  llvm::Value *derived = this->lowering.builder_.CreateLoad(
      ptr, this->lowering.builder_.CreateStructGEP(type, object, kReactionDerived));
  llvm::Value *callback = this->lowering.builder_.CreateLoad(
      this->lowering.fnTy_,
      this->lowering.builder_.CreateStructGEP(type, object, kReactionCallback));
  if (this->lowering.memory().hasCycles) {
    this->lowering.storeLowerer().releaseIn(object, derived, Type::promise(Type::voidTy()));
    this->lowering.storeLowerer().releaseIn(object, callback,
                                            Type::function({}, 0, false, Type::voidTy()));
    this->lowering.builder_.CreateCall(this->lowering.groupLowerer().groupFunction(
                                           "tz_group_obj_free", this->lowering.builder_.getVoidTy(),
                                           {this->lowering.builder_.getPtrTy()}),
                                       {object});
  } else {
    this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {derived});
    this->lowering.builder_.CreateCall(this->lowering.objReleaseFn_,
                                       {this->lowering.builder_.CreateExtractValue(callback, 1)});
    this->lowering.builder_.CreateCall(this->lowering.rt_.objFree, {object});
  }
  this->lowering.builder_.CreateRetVoid();
  this->lowering.builder_.restoreIP(point);

  return this->reactionDropFn;
}

llvm::Function *ReactionLowerer::reactionHandler(const Type *value, const Type *result,
                                                 const Type *callback) {
  llvm::Function *&handler = this->lowering.reactionHandlers_[{value, result, callback}];
  if (handler) {
    return handler;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  handler = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.then", this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(handler);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
  llvm::Function *outer = std::exchange(this->lowering.function_, handler);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", handler));
  llvm::StructType *type = this->reactionType();
  llvm::Value *object = this->lowering.builder_.CreateInBoundsGEP(
      this->lowering.builder_.getInt8Ty(), handler->getArg(0),
      llvm::ConstantInt::getSigned(this->lowering.builder_.getInt64Ty(), -int64_t(kReactorOffset)),
      "reaction");
  llvm::Value *source = handler->getArg(1);
  llvm::Value *derived = this->lowering.builder_.CreateLoad(
      ptr, this->lowering.builder_.CreateStructGEP(type, object, kReactionDerived), "derived");
  bool holds = this->lowering.promiseLowerer().promiseValueSize(result) != 0;
  llvm::Value *given = nullptr;
  if (this->lowering.promiseLowerer().promiseValueSize(value)) {
    given = this->lowering.builder_.CreateLoad(
        this->lowering.moduleBuilder().lower(value),
        this->lowering.promiseLowerer().promiseValueAddress(source), "value");
    this->lowering.refCounts().retainValue(given, value);
  }
  if (!callback) {
    this->lowering.promiseLowerer().fulfillPromise(
        derived,
        holds && given ? this->lowering.exprLowerer().emitConversion(given, value, result)
                       : nullptr,
        result);
  } else {
    llvm::Value *fn = this->lowering.builder_.CreateLoad(
        this->lowering.fnTy_,
        this->lowering.builder_.CreateStructGEP(type, object, kReactionCallback), "callback");
    std::vector<llvm::Value *> args;
    if (given && !callback->params().empty()) {
      args.push_back(given);
    } else if (given) {
      this->lowering.refCounts().releaseValue(given, value);
    }
    llvm::Value *context = nullptr;
    llvm::Value *outerContext = nullptr;
    if (this->lowering.contexts()) {
      llvm::Value *slot = this->lowering.builder_.CreateStructGEP(type, object, kReactorContext);
      context = this->lowering.builder_.CreateLoad(ptr, slot, "context");
      this->lowering.builder_.CreateStore(
          llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), slot);
      outerContext = this->lowering.builder_.CreateCall(
          this->lowering.asyncLowerer().asyncFunction("tz_context_resume", ptr, {ptr}), {context},
          "outer");
    }
    llvm::Value *gave = this->lowering.closureLowerer().callFunctionValue(fn, callback, args);
    if (context) {
      this->lowering.builder_.CreateCall(
          this->lowering.asyncLowerer().asyncFunction(
              "tz_context_done", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
          {outerContext, context});
    }
    const Type *ret = callback->ret();
    if (ret->isPromise()) {
      this->lowering.promiseLowerer().followPromise(derived, gave);
    } else {
      llvm::Value *converted = nullptr;
      if (holds && !ret->isVoid()) {
        converted = this->lowering.exprLowerer().emitConversion(gave, ret, result);
      }
      this->lowering.promiseLowerer().fulfillPromise(derived, converted, result);
    }
  }
  this->lowering.builder_.CreateRetVoid();
  this->lowering.function_ = outer;
  this->lowering.builder_.restoreIP(point);

  return handler;
}

llvm::Value *ReactionLowerer::emitPromiseReaction(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *promiseType = member->object->type;
  const Type *value = promiseType->promised();
  const Type *result = call->type->promised();
  std::vector<Expr *> exprs{member->object};
  exprs.insert(exprs.end(), call->args.begin(), call->args.end());
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(exprs);
  llvm::Value *source = values[0];
  const Type *callbackType = nullptr;
  llvm::Value *callback = nullptr;
  if (!call->args.empty() && call->args[0]->type->isFunction()) {
    callbackType = call->args[0]->type;
    callback = values[1];
  }
  llvm::Value *derived = this->lowering.promiseLowerer().newPromise(result);
  this->attachReaction(source, derived, this->reactionHandler(value, result, callbackType),
                       callback);
  this->lowering.refCounts().releaseValue(source, promiseType);

  return derived;
}

void ReactionLowerer::attachReaction(llvm::Value *source, llvm::Value *derived,
                                     llvm::Function *handler, llvm::Value *callback, bool later) {
  llvm::StructType *type = this->reactionType();
  uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(type);
  llvm::Value *object =
      this->lowering.memory().hasCycles
          ? this->lowering.groupLowerer().allocMember(size, "reaction")
          : this->lowering.builder_.CreateCall(
                this->lowering.rt_.objAlloc, {this->lowering.builder_.getInt64(size)}, "reaction");
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  if (!this->lowering.memory().hasCycles) {
    this->lowering.builder_.CreateStore(
        this->lowering.builder_.getInt64(1),
        this->lowering.builder_.CreateStructGEP(type, object, kReactionRc));
  }
  this->lowering.builder_.CreateStore(
      this->lowering.memory().hasCycles
          ? this->lowering.groupLowerer().memberDescriptor(this->reactionDrop(), "reaction")
          : this->lowering.classDescriptors().dropDescriptor(this->reactionDrop()),
      this->lowering.builder_.CreateStructGEP(type, object, kReactionDesc));
  this->lowering.builder_.CreateStore(
      null, this->lowering.builder_.CreateStructGEP(type, object, kReactorNext));
  this->lowering.builder_.CreateStore(
      handler, this->lowering.builder_.CreateStructGEP(type, object, kReactorRun));
  this->lowering.builder_.CreateStore(
      object, this->lowering.builder_.CreateStructGEP(type, object, kReactorOwner));
  this->lowering.builder_.CreateStore(
      null, this->lowering.builder_.CreateStructGEP(type, object, kReactorSource));
  this->lowering.builder_.CreateStore(
      this->lowering.builder_.getInt64(0),
      this->lowering.builder_.CreateStructGEP(type, object, kReactorAhead));
  llvm::Value *context = callback && this->lowering.contexts()
                             ? this->lowering.builder_.CreateCall(
                                   this->lowering.asyncLowerer().asyncFunction(
                                       "tz_context_hold", this->lowering.builder_.getPtrTy(), {}),
                                   {}, "context")
                             : null;
  this->lowering.builder_.CreateStore(
      context, this->lowering.builder_.CreateStructGEP(type, object, kReactorContext));
  this->lowering.builder_.CreateCall(this->lowering.objRetainFn_, {derived});
  this->lowering.builder_.CreateStore(
      derived, this->lowering.builder_.CreateStructGEP(type, object, kReactionDerived));
  this->lowering.builder_.CreateStore(
      callback ? callback : llvm::Constant::getNullValue(this->lowering.fnTy_),
      this->lowering.builder_.CreateStructGEP(type, object, kReactionCallback));
  if (this->lowering.memory().hasCycles) {
    this->lowering.storeLowerer().groupStore(object, derived, Type::promise(Type::voidTy()));
    if (callback) {
      this->lowering.storeLowerer().groupStore(object, callback,
                                               Type::function({}, 0, false, Type::voidTy()));
    }
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->lowering.builder_.CreateCall(
      this->lowering.asyncLowerer().asyncFunction(later ? "tz_promise_react_later"
                                                        : "tz_promise_react",
                                                  this->lowering.builder_.getVoidTy(), {ptr, ptr}),
      {source, this->lowering.builder_.CreateStructGEP(type, object, kReactorNext)});
}

}
