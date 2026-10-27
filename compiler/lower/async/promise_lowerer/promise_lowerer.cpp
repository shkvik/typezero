#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_combine_lowerer/promise_combine_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
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

constexpr uint64_t kPromiseHeader = 48;

constexpr uint32_t kLayoutPromise = 131072;

}

PromiseLowerer::PromiseLowerer(Lowering &lowering) : lowering_(lowering) {}

uint64_t PromiseLowerer::promiseValueSize(const Type *value) {
  if (value->isVoid() || value->isNever() || value->isUndefined()) {
    return 0;
  }
  uint64_t size =
      lowering_.module_->getDataLayout().getTypeAllocSize(lowering_.moduleBuilder().lower(value));

  return (size + 7) / 8 * 8;
}

llvm::Value *PromiseLowerer::promiseValueAddress(llvm::Value *promise) {
  return lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), promise,
                                                       kPromiseHeader, "promise.value");
}

llvm::Constant *PromiseLowerer::promiseLayout(const Type *value) {
  llvm::Constant *&layout = lowering_.promiseLayouts_[value];
  if (layout) {
    return layout;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Constant *fields = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  uint32_t count = 0;
  if (promiseValueSize(value)) {
    auto *field = new llvm::GlobalVariable(
        *lowering_.module_, lowering_.fieldDescTy_, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(lowering_.fieldDescTy_,
                                  {lowering_.stringLowerer().utf8Constant(""),
                                   lowering_.builder_.getInt32(0),
                                   lowering_.builder_.getInt32(uint32_t(kPromiseHeader)),
                                   lowering_.typeDescriptors().typeDescriptor(value)}),
        "tz.promise.field");
    field->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = field;
    count = 1;
  }
  auto *drop = cast<llvm::Function>(
      lowering_.asyncLowerer()
          .asyncFunction("tz_promise_drop", lowering_.builder_.getVoidTy(), {ptr})
          .getCallee());
  uint32_t flags = kLayoutPromise | (lowering_.memory().promiseIsMember(value) ? 0x80000000u : 0u);
  auto *global = new llvm::GlobalVariable(
      *lowering_.module_, lowering_.layoutDescTy_, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(lowering_.layoutDescTy_,
                                {drop, fields, lowering_.builder_.getInt32(count),
                                 lowering_.builder_.getInt32(flags),
                                 llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                 lowering_.builder_.getInt32(0),
                                 lowering_.builder_.getInt32(
                                     lowering_.unionLowerer().plainTypeId(Type::promise(value)))}),
      "tz.layout.promise");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  layout = global;

  return layout;
}

llvm::Value *PromiseLowerer::newPromise(const Type *value) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto *fn = cast<llvm::Function>(
      lowering_.asyncLowerer().asyncFunction("tz_promise_new", ptr, {ptr}).getCallee());
  fn->addRetAttr(llvm::Attribute::NoAlias);

  return lowering_.builder_.CreateCall(fn, {promiseLayout(value)}, "promise");
}

void PromiseLowerer::fulfillPromise(llvm::Value *promise, llvm::Value *value, const Type *type) {
  if (value && promiseValueSize(type)) {
    lowering_.builder_.CreateStore(value, promiseValueAddress(promise));
  }
  lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_promise_fulfill", lowering_.builder_.getVoidTy(),
                                             {lowering_.builder_.getPtrTy()}),
      {promise});
}

void PromiseLowerer::rejectPromise(llvm::Value *promise, llvm::Value *error) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_promise_reject", lowering_.builder_.getVoidTy(),
                                             {ptr, ptr}),
      {promise, error});
}

void PromiseLowerer::followPromise(llvm::Value *promise, llvm::Value *other) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_promise_follow", lowering_.builder_.getVoidTy(),
                                             {ptr, ptr}),
      {promise, other});
}

llvm::Value *PromiseLowerer::promiseField(llvm::Value *promise, unsigned field) {
  return lowering_.builder_.CreateStructGEP(lowering_.promiseObjTy_, promise, field);
}

llvm::Constant *PromiseLowerer::resolverInfo(const Type *value) {
  llvm::Constant *&info = lowering_.resolverInfos_[value];
  if (info) {
    return info;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  bool holds = promiseValueSize(value) != 0;
  std::vector<llvm::Type *> params{ptr};
  if (!value->isVoid()) {
    params.push_back(lowering_.moduleBuilder().lower(value));
  }
  auto *code = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), params, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.resolve", lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, code);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", code));
  llvm::Value *promise = code->getArg(0);
  llvm::Value *locked =
      lowering_.builder_.CreateCall(lowering_.asyncLowerer().asyncFunction(
                                        "tz_promise_lock", lowering_.builder_.getInt32Ty(), {ptr}),
                                    {promise}, "locked");
  llvm::BasicBlock *settleBlock = lowering_.functionFrame().newBlock("settle");
  llvm::BasicBlock *dropBlock = lowering_.functionFrame().newBlock("drop");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(locked), settleBlock,
                                  dropBlock);
  lowering_.functionFrame().switchTo(settleBlock);
  fulfillPromise(promise, holds ? code->getArg(1) : nullptr, value);
  lowering_.builder_.CreateRetVoid();
  lowering_.functionFrame().switchTo(dropBlock);
  if (!value->isVoid()) {
    lowering_.refCounts().releaseValue(code->getArg(1), value);
  }
  lowering_.builder_.CreateRetVoid();
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);
  info = new llvm::GlobalVariable(
      *lowering_.module_, lowering_.fnInfoTy_, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(lowering_.fnInfoTy_,
                                {code, lowering_.stringLowerer().stringLiteral(u"")}),
      "tz.fninfo.resolve");

  return info;
}

llvm::Constant *PromiseLowerer::rejecterInfo() {
  if (rejecterInfo_) {
    return rejecterInfo_;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto *code = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.reject", lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, code);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", code));
  llvm::Value *promise = code->getArg(0), *error = code->getArg(1);
  llvm::Value *locked =
      lowering_.builder_.CreateCall(lowering_.asyncLowerer().asyncFunction(
                                        "tz_promise_lock", lowering_.builder_.getInt32Ty(), {ptr}),
                                    {promise}, "locked");
  llvm::BasicBlock *settleBlock = lowering_.functionFrame().newBlock("settle");
  llvm::BasicBlock *dropBlock = lowering_.functionFrame().newBlock("drop");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(locked), settleBlock,
                                  dropBlock);
  lowering_.functionFrame().switchTo(settleBlock);
  rejectPromise(promise, error);
  lowering_.builder_.CreateRetVoid();
  lowering_.functionFrame().switchTo(dropBlock);
  lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {error});
  lowering_.builder_.CreateRetVoid();
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);
  rejecterInfo_ = new llvm::GlobalVariable(
      *lowering_.module_, lowering_.fnInfoTy_, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(lowering_.fnInfoTy_,
                                {code, lowering_.stringLowerer().stringLiteral(u"")}),
      "tz.fninfo.reject");

  return rejecterInfo_;
}

llvm::Value *PromiseLowerer::emitPromiseNew(CallExpr *call) {
  const Type *value = call->type->promised();
  Expr *executorExpr = call->args[0];
  const Type *fnType = executorExpr->type;
  llvm::Value *executor = lowering_.exprLowerer().emitExpr(executorExpr);
  llvm::Value *promise = newPromise(value);
  std::vector<llvm::Value *> args;
  for (int i = 0; i < 2; ++i) {
    lowering_.builder_.CreateCall(lowering_.objRetainFn_, {promise});
    args.push_back(lowering_.closureLowerer().makeFunctionValue(
        i == 0 ? resolverInfo(value) : rejecterInfo(), promise));
  }
  size_t takes = std::min<size_t>(fnType->params().size(), 2);
  for (size_t i = takes; i < args.size(); ++i) {
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {promise});
  }
  args.resize(takes);
  lowering_.closureLowerer().callFunctionValue(executor, fnType, args);
  bool throws = lowering_.typeThrows(fnType);
  if (auto *fe = dyn_cast<FunctionExpr>(executorExpr)) {
    throws = lowering_.mayThrow(fe->fn);
  }
  if (throws) {
    llvm::Value *error = lowering_.builder_.CreateLoad(
        lowering_.builder_.getPtrTy(), lowering_.throwLowerer().thrownSlot(), "thrown");
    llvm::BasicBlock *rejectBlock = lowering_.functionFrame().newBlock("executor.threw");
    llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("executor.done");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(error), rejectBlock,
                                    doneBlock,
                                    llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
    lowering_.functionFrame().switchTo(rejectBlock);
    lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                   lowering_.throwLowerer().thrownSlot());
    llvm::Value *locked = lowering_.builder_.CreateCall(
        lowering_.asyncLowerer().asyncFunction("tz_promise_lock", lowering_.builder_.getInt32Ty(),
                                               {lowering_.builder_.getPtrTy()}),
        {promise}, "locked");
    llvm::BasicBlock *settleBlock = lowering_.functionFrame().newBlock("executor.reject");
    llvm::BasicBlock *dropBlock = lowering_.functionFrame().newBlock("executor.drop");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(locked), settleBlock,
                                    dropBlock);
    lowering_.functionFrame().switchTo(settleBlock);
    rejectPromise(promise, error);
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(dropBlock);
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {error});
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(doneBlock);
  }
  lowering_.refCounts().releaseValue(executor, fnType);

  return promise;
}

llvm::Value *PromiseLowerer::emitPromiseStatic(CallExpr *call) {
  const Type *type = call->type;
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  switch (call->builtin) {
  case Builtin::PromiseResolve: {
    if (!call->args.empty() && call->args[0]->type->isPromise()) {
      return lowering_.exprLowerer().emitExpr(call->args[0]);
    }
    llvm::Value *value = nullptr;
    if (!call->args.empty()) {
      value = lowering_.exprLowerer().emitExpr(call->args[0]);
    }
    auto *fn = cast<llvm::Function>(
        lowering_.asyncLowerer().asyncFunction("tz_promise_resolved", ptr, {ptr}).getCallee());
    llvm::Value *promise =
        lowering_.builder_.CreateCall(fn, {promiseLayout(type->promised())}, "promise");
    if (value && promiseValueSize(type->promised())) {
      lowering_.builder_.CreateStore(value, promiseValueAddress(promise));
    }
    return promise;
  }
  case Builtin::PromiseReject: {
    llvm::Value *error = lowering_.exprLowerer().emitExpr(call->args[0]);
    lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_error_rejected",
                                                   lowering_.builder_.getVoidTy(), {ptr, ptr}),
        {error, lowering_.unwindLowerer().frameDescriptor(lowering_.loc_)});
    return lowering_.builder_.CreateCall(
        lowering_.asyncLowerer().asyncFunction("tz_promise_rejected", ptr, {ptr, ptr}),
        {promiseLayout(type->promised()), error}, "promise");
  }
  case Builtin::PromiseWithResolvers: {
    const Type *promiseType = type->field("promise")->type;
    const Type *value = promiseType->promised();
    llvm::Value *promise = newPromise(value);
    bool rejects = type->field("reject") != nullptr;
    llvm::Value *object = rejects ? lowering_.objectLayouts().allocObject(type, {0, 1, 2})
                                  : lowering_.objectLayouts().allocObject(type, {0, 1});
    lowering_.builder_.CreateStore(promise,
                                   lowering_.objectLayouts().fieldAddress(object, type, "promise"));
    for (int i = 0; i < (rejects ? 2 : 1); ++i) {
      lowering_.builder_.CreateCall(lowering_.objRetainFn_, {promise});
      lowering_.builder_.CreateStore(
          lowering_.closureLowerer().makeFunctionValue(
              i == 0 ? resolverInfo(value) : rejecterInfo(), promise),
          lowering_.objectLayouts().fieldAddress(object, type, i == 0 ? "resolve" : "reject"));
    }
    lowering_.groupLowerer().adoptFields(object, type);
    return object;
  }
  default:
    return lowering_.promiseCombineLowerer().emitPromiseCombine(call);
  }
}

}
