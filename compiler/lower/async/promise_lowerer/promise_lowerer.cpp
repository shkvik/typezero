#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_combine_lowerer/promise_combine_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
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

// Must match runtime/core/tzrt/tzrt.h.
constexpr uint64_t kPromiseHeader = 48; // TZ_PROMISE_VALUE

constexpr uint32_t kLayoutPromise = 131072; // TZ_LAYOUT_PROMISE

} // namespace

PromiseLowerer::PromiseLowerer(CodeGen &codegen) : codegen_(codegen) {}

uint64_t PromiseLowerer::promiseValueSize(const Type *value) {
  if (value->isVoid() || value->isNever() || value->isUndefined()) {
    return 0;
  }
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(codegen_.moduleBuilder().lower(value));

  return (size + 7) / 8 * 8;
}

llvm::Value *PromiseLowerer::promiseValueAddress(llvm::Value *promise) {
  return codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), promise, kPromiseHeader,
                                             "promise.value");
}

llvm::Constant *PromiseLowerer::promiseLayout(const Type *value) {
  llvm::Constant *&layout = codegen_.promiseLayouts_[value];
  if (layout) {
    return layout;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Constant *fields = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  uint32_t count = 0;
  if (promiseValueSize(value)) {
    auto *field = new llvm::GlobalVariable(
        *codegen_.module_, codegen_.fieldDescTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(codegen_.fieldDescTy_, {codegen_.stringLowerer().utf8Constant(""), codegen_.builder_.getInt32(0),
                                                 codegen_.builder_.getInt32(uint32_t(kPromiseHeader)),
                                                 codegen_.typeDescriptors().typeDescriptor(value)}),
        "tz.promise.field");
    field->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = field;
    count = 1;
  }
  auto *drop = cast<llvm::Function>(
      codegen_.asyncLowerer().asyncFunction("tz_promise_drop", codegen_.builder_.getVoidTy(), {ptr}).getCallee());
  // TZ_LAYOUT_GROUP: its promises are members of groups
  // (runtime/core/async/async.c).
  uint32_t flags = kLayoutPromise | (codegen_.promiseIsMember(value) ? 0x80000000u : 0u);
  auto *global = new llvm::GlobalVariable(
      *codegen_.module_, codegen_.layoutDescTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          codegen_.layoutDescTy_, {drop, fields, codegen_.builder_.getInt32(count), codegen_.builder_.getInt32(flags),
                          llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), codegen_.builder_.getInt32(0),
                          codegen_.builder_.getInt32(codegen_.unionLowerer().plainTypeId(Type::promise(value)))}),
      "tz.layout.promise");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  layout = global;

  return layout;
}

llvm::Value *PromiseLowerer::newPromise(const Type *value) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *fn = cast<llvm::Function>(codegen_.asyncLowerer().asyncFunction("tz_promise_new", ptr, {ptr}).getCallee());
  fn->addRetAttr(llvm::Attribute::NoAlias);

  return codegen_.builder_.CreateCall(fn, {promiseLayout(value)}, "promise");
}

void PromiseLowerer::fulfillPromise(llvm::Value *promise, llvm::Value *value, const Type *type) {
  if (value && promiseValueSize(type)) {
    codegen_.builder_.CreateStore(value, promiseValueAddress(promise));
  }
  codegen_.builder_.CreateCall(
      codegen_.asyncLowerer().asyncFunction("tz_promise_fulfill", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}), {promise});
}

void PromiseLowerer::rejectPromise(llvm::Value *promise, llvm::Value *error) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_promise_reject", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {promise, error});
}

void PromiseLowerer::followPromise(llvm::Value *promise, llvm::Value *other) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_promise_follow", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {promise, other});
}

llvm::Value *PromiseLowerer::promiseField(llvm::Value *promise, unsigned field) {
  return codegen_.builder_.CreateStructGEP(codegen_.promiseObjTy_, promise, field);
}

// void (ptr promise, T value): `resolve` of `new Promise`, whose environment
// is the promise. Only the first of resolve and reject counts.
llvm::Constant *PromiseLowerer::resolverInfo(const Type *value) {
  llvm::Constant *&info = codegen_.resolverInfos_[value];
  if (info) {
    return info;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  bool holds = promiseValueSize(value) != 0;
  std::vector<llvm::Type *> params{ptr};
  if (!value->isVoid()) {
    params.push_back(codegen_.moduleBuilder().lower(value));
  }
  auto *code = llvm::Function::Create(llvm::FunctionType::get(codegen_.builder_.getVoidTy(), params, false),
                                      llvm::GlobalValue::InternalLinkage, "tz.promise.resolve",
                                      codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = std::exchange(codegen_.function_, code);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
  llvm::Value *promise = code->getArg(0);
  llvm::Value *locked = codegen_.builder_.CreateCall(
      codegen_.asyncLowerer().asyncFunction("tz_promise_lock", codegen_.builder_.getInt32Ty(), {ptr}), {promise}, "locked");
  llvm::BasicBlock *settleBlock = codegen_.functionFrame().newBlock("settle");
  llvm::BasicBlock *dropBlock = codegen_.functionFrame().newBlock("drop");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(locked), settleBlock, dropBlock);
  codegen_.functionFrame().switchTo(settleBlock);
  fulfillPromise(promise, holds ? code->getArg(1) : nullptr, value);
  codegen_.builder_.CreateRetVoid();
  codegen_.functionFrame().switchTo(dropBlock);
  if (!value->isVoid()) {
    codegen_.refCounts().releaseValue(code->getArg(1), value);
  }
  codegen_.builder_.CreateRetVoid();
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);
  info = new llvm::GlobalVariable(
      *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(codegen_.fnInfoTy_, {code, codegen_.stringLowerer().stringLiteral(u"")}), "tz.fninfo.resolve");

  return info;
}

// void (ptr promise, ptr error): `reject`.
llvm::Constant *PromiseLowerer::rejecterInfo() {
  if (rejecterInfo_) {
    return rejecterInfo_;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *code = llvm::Function::Create(
      llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.reject", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = std::exchange(codegen_.function_, code);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
  llvm::Value *promise = code->getArg(0), *error = code->getArg(1);
  llvm::Value *locked = codegen_.builder_.CreateCall(
      codegen_.asyncLowerer().asyncFunction("tz_promise_lock", codegen_.builder_.getInt32Ty(), {ptr}), {promise}, "locked");
  llvm::BasicBlock *settleBlock = codegen_.functionFrame().newBlock("settle");
  llvm::BasicBlock *dropBlock = codegen_.functionFrame().newBlock("drop");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(locked), settleBlock, dropBlock);
  codegen_.functionFrame().switchTo(settleBlock);
  rejectPromise(promise, error);
  codegen_.builder_.CreateRetVoid();
  codegen_.functionFrame().switchTo(dropBlock);
  codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {error});
  codegen_.builder_.CreateRetVoid();
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);
  rejecterInfo_ = new llvm::GlobalVariable(
      *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(codegen_.fnInfoTy_, {code, codegen_.stringLowerer().stringLiteral(u"")}), "tz.fninfo.reject");

  return rejecterInfo_;
}

// The executor runs now with the two functions; an error it throws rejects
// the promise, unless it is settled already.
llvm::Value *PromiseLowerer::emitPromiseNew(CallExpr *call) {
  const Type *value = call->type->promised();
  Expr *executorExpr = call->args[0];
  const Type *fnType = executorExpr->type;
  llvm::Value *executor = codegen_.exprLowerer().emitExpr(executorExpr);
  llvm::Value *promise = newPromise(value);
  std::vector<llvm::Value *> args;
  // The promise may be a member of a group (promiseIsMember): counted as
  // one.
  for (int i = 0; i < 2; ++i) {
    codegen_.builder_.CreateCall(codegen_.objRetainFn_, {promise});
    args.push_back(codegen_.closureLowerer().makeFunctionValue(i == 0 ? resolverInfo(value) : rejecterInfo(), promise));
  }
  // An executor that names fewer parameters gets fewer.
  size_t takes = std::min<size_t>(fnType->params().size(), 2);
  for (size_t i = takes; i < args.size(); ++i) {
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {promise});
  }
  args.resize(takes);
  codegen_.closureLowerer().callFunctionValue(executor, fnType, args);
  bool throws = codegen_.typeThrows(fnType);
  if (auto *fe = dyn_cast<FunctionExpr>(executorExpr)) {
    throws = codegen_.mayThrow(fe->fn);
  }
  if (throws) {
    llvm::Value *error = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.throwLowerer().thrownSlot(), "thrown");
    llvm::BasicBlock *rejectBlock = codegen_.functionFrame().newBlock("executor.threw");
    llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("executor.done");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(error), rejectBlock, doneBlock,
                          llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
    codegen_.functionFrame().switchTo(rejectBlock);
    codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), codegen_.throwLowerer().thrownSlot());
    // As reject does: nothing once the promise is settled.
    llvm::Value *locked = codegen_.builder_.CreateCall(
        codegen_.asyncLowerer().asyncFunction("tz_promise_lock", codegen_.builder_.getInt32Ty(), {codegen_.builder_.getPtrTy()}), {promise},
        "locked");
    llvm::BasicBlock *settleBlock = codegen_.functionFrame().newBlock("executor.reject");
    llvm::BasicBlock *dropBlock = codegen_.functionFrame().newBlock("executor.drop");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(locked), settleBlock, dropBlock);
    codegen_.functionFrame().switchTo(settleBlock);
    rejectPromise(promise, error);
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(dropBlock);
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {error});
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(doneBlock);
  }
  codegen_.refCounts().releaseValue(executor, fnType);

  return promise;
}

llvm::Value *PromiseLowerer::emitPromiseStatic(CallExpr *call) {
  const Type *type = call->type;
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  switch (call->builtin) {
  case Builtin::PromiseResolve: {
    if (!call->args.empty() && call->args[0]->type->isPromise()) {
      return codegen_.exprLowerer().emitExpr(call->args[0]); // that very promise
    }
    llvm::Value *value = nullptr;
    if (!call->args.empty()) {
      value = codegen_.exprLowerer().emitExpr(call->args[0]);
    }
    auto *fn = cast<llvm::Function>(codegen_.asyncLowerer().asyncFunction("tz_promise_resolved", ptr, {ptr}).getCallee());
    llvm::Value *promise = codegen_.builder_.CreateCall(fn, {promiseLayout(type->promised())}, "promise");
    if (value && promiseValueSize(type->promised())) {
      codegen_.builder_.CreateStore(value, promiseValueAddress(promise));
    }
    return promise;
  }
  case Builtin::PromiseReject: {
    llvm::Value *error = codegen_.exprLowerer().emitExpr(call->args[0]);
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_error_rejected", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                        {error, codegen_.unwindLowerer().frameDescriptor(codegen_.loc_)});
    return codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_promise_rejected", ptr, {ptr, ptr}),
                               {promiseLayout(type->promised()), error}, "promise");
  }
  case Builtin::PromiseWithResolvers: {
    const Type *promiseType = type->field("promise")->type;
    const Type *value = promiseType->promised();
    llvm::Value *promise = newPromise(value);
    llvm::Value *object = codegen_.objectLayouts().allocObject(type, {0, 1, 2});
    codegen_.builder_.CreateStore(promise, codegen_.objectLayouts().fieldAddress(object, type, "promise"));
    for (int i = 0; i < 2; ++i) {
      codegen_.builder_.CreateCall(codegen_.objRetainFn_, {promise});
      codegen_.builder_.CreateStore(
          codegen_.closureLowerer().makeFunctionValue(i == 0 ? resolverInfo(value) : rejecterInfo(), promise),
          codegen_.objectLayouts().fieldAddress(object, type, i == 0 ? "resolve" : "reject"));
    }
    codegen_.groupLowerer().adoptFields(object, type);
    return object;
  }
  default:
    return codegen_.promiseCombineLowerer().emitPromiseCombine(call);
  }
}

} // namespace tz
