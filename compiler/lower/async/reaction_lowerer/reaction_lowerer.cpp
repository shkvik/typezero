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

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

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
  kReactionDerived,
  kReactionCallback,
};

constexpr uint64_t kReactorOffset = 16;

}

ReactionLowerer::ReactionLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::StructType *ReactionLowerer::reactionType() {
  if (!reactionTy_) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy();
    reactionTy_ =
        llvm::StructType::create(lowering_.ctx_,
                                 {lowering_.builder_.getInt64Ty(), ptr, ptr, ptr, ptr, ptr,
                                  lowering_.builder_.getInt64Ty(), ptr, lowering_.fnTy_},
                                 "tz.reaction");
  }

  return reactionTy_;
}

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
  llvm::Value *callback = lowering_.builder_.CreateLoad(
      lowering_.fnTy_, lowering_.builder_.CreateStructGEP(type, object, kReactionCallback));
  if (lowering_.memory().hasCycles) {
    lowering_.storeLowerer().releaseIn(object, derived, Type::promise(Type::voidTy()));
    lowering_.storeLowerer().releaseIn(object, callback,
                                       Type::function({}, 0, false, Type::voidTy()));
    lowering_.builder_.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_group_obj_free", lowering_.builder_.getVoidTy(),
                                               {lowering_.builder_.getPtrTy()}),
        {object});
  } else {
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {derived});
    lowering_.builder_.CreateCall(lowering_.objReleaseFn_,
                                  {lowering_.builder_.CreateExtractValue(callback, 1)});
    lowering_.builder_.CreateCall(lowering_.rt_.objFree, {object});
  }
  lowering_.builder_.CreateRetVoid();
  lowering_.builder_.restoreIP(point);

  return reactionDropFn_;
}

llvm::Function *ReactionLowerer::reactionHandler(const Type *value, const Type *result,
                                                 const Type *callback) {
  llvm::Function *&handler = lowering_.reactionHandlers_[{value, result, callback}];
  if (handler) {
    return handler;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  handler = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.then", lowering_.module_.get());
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
  llvm::Value *given = nullptr;
  if (lowering_.promiseLowerer().promiseValueSize(value)) {
    given = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(value),
                                          lowering_.promiseLowerer().promiseValueAddress(source),
                                          "value");
    lowering_.refCounts().retainValue(given, value);
  }
  if (!callback) {
    lowering_.promiseLowerer().fulfillPromise(
        derived,
        holds && given ? lowering_.exprLowerer().emitConversion(given, value, result) : nullptr,
        result);
  } else {
    llvm::Value *fn = lowering_.builder_.CreateLoad(
        lowering_.fnTy_, lowering_.builder_.CreateStructGEP(type, object, kReactionCallback),
        "callback");
    std::vector<llvm::Value *> args;
    if (given && !callback->params().empty()) {
      args.push_back(given);
    } else if (given) {
      lowering_.refCounts().releaseValue(given, value);
    }
    llvm::Value *gave = lowering_.closureLowerer().callFunctionValue(fn, callback, args);
    const Type *ret = callback->ret();
    if (ret->isPromise()) {
      lowering_.promiseLowerer().followPromise(derived, gave);
    } else {
      llvm::Value *converted = nullptr;
      if (holds && !ret->isVoid()) {
        converted = lowering_.exprLowerer().emitConversion(gave, ret, result);
      }
      lowering_.promiseLowerer().fulfillPromise(derived, converted, result);
    }
  }
  lowering_.builder_.CreateRetVoid();
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
  std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(exprs);
  llvm::Value *source = values[0];
  const Type *callbackType = nullptr;
  llvm::Value *callback = nullptr;
  if (!call->args.empty() && call->args[0]->type->isFunction()) {
    callbackType = call->args[0]->type;
    callback = values[1];
  }
  llvm::Value *derived = lowering_.promiseLowerer().newPromise(result);
  attachReaction(source, derived, reactionHandler(value, result, callbackType), callback);
  lowering_.refCounts().releaseValue(source, promiseType);

  return derived;
}

void ReactionLowerer::attachReaction(llvm::Value *source, llvm::Value *derived,
                                     llvm::Function *handler, llvm::Value *callback, bool later) {
  llvm::StructType *type = reactionType();
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(type);
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
  lowering_.builder_.CreateStore(
      callback ? callback : llvm::Constant::getNullValue(lowering_.fnTy_),
      lowering_.builder_.CreateStructGEP(type, object, kReactionCallback));
  if (lowering_.memory().hasCycles) {
    lowering_.storeLowerer().groupStore(object, derived, Type::promise(Type::voidTy()));
    if (callback) {
      lowering_.storeLowerer().groupStore(object, callback,
                                          Type::function({}, 0, false, Type::voidTy()));
    }
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction(later ? "tz_promise_react_later" : "tz_promise_react",
                                             lowering_.builder_.getVoidTy(), {ptr, ptr}),
      {source, lowering_.builder_.CreateStructGEP(type, object, kReactorNext)});
}

}
