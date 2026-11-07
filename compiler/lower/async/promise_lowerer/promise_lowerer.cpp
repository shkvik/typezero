#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_combine_lowerer/promise_combine_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/places/place_lowerer/place_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;

namespace tz {

namespace {

constexpr uint64_t kPromiseHeader = 40;

constexpr uint32_t kLayoutPromise = 131072;

}

PromiseLowerer::PromiseLowerer(Lowering &owner)
    : lowering(owner) {}

uint64_t PromiseLowerer::promiseValueSize(const Type *value) {
  if (value->isVoid() || value->isNever() || value->isUndefined()) {
    return 0;
  }
  uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(
      this->lowering.moduleBuilder().lower(value));

  return (size + 7) / 8 * 8;
}

llvm::Value *PromiseLowerer::promiseValueAddress(llvm::Value *promise) {
  return this->lowering.builder_.CreateConstInBoundsGEP1_64(
      this->lowering.builder_.getInt8Ty(), promise, kPromiseHeader, "promise.value");
}

llvm::Constant *PromiseLowerer::promiseLayout(const Type *value) {
  llvm::Constant *&layout = this->lowering.promiseLayouts_[value];
  if (layout) {
    return layout;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Constant *fields = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  uint32_t count = 0;
  if (this->promiseValueSize(value)) {
    auto *field = new llvm::GlobalVariable(
        *this->lowering.module_, this->lowering.fieldDescTy_, true,
        llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(this->lowering.fieldDescTy_,
                                  {this->lowering.stringLowerer().utf8Constant(""),
                                   this->lowering.builder_.getInt32(0),
                                   this->lowering.builder_.getInt32(uint32_t(kPromiseHeader)),
                                   this->lowering.typeDescriptors().typeDescriptor(value)}),
        "tz.promise.field");
    field->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = field;
    count = 1;
  }
  auto *drop = cast<llvm::Function>(
      this->lowering.asyncLowerer()
          .asyncFunction("tz_promise_drop", this->lowering.builder_.getVoidTy(), {ptr})
          .getCallee());
  uint32_t flags =
      kLayoutPromise | (this->lowering.memory().promiseIsMember(value) ? 0x80000000u : 0u);
  auto *global = new llvm::GlobalVariable(
      *this->lowering.module_, this->lowering.layoutDescTy_, true,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          this->lowering.layoutDescTy_,
          {drop, fields, this->lowering.builder_.getInt32(count),
           this->lowering.builder_.getInt32(flags),
           llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()),
           this->lowering.builder_.getInt32(0),
           this->lowering.builder_.getInt32(
               this->lowering.unionLowerer().plainTypeId(Type::promise(value)))}),
      "tz.layout.promise");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  layout = global;

  return layout;
}

llvm::Value *PromiseLowerer::newPromise(const Type *value) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  auto *fn = cast<llvm::Function>(
      this->lowering.asyncLowerer().asyncFunction("tz_promise_new", ptr, {ptr}).getCallee());
  fn->addRetAttr(llvm::Attribute::NoAlias);

  return this->lowering.builder_.CreateCall(fn, {this->promiseLayout(value)}, "promise");
}

void PromiseLowerer::fulfillPromise(llvm::Value *promise, llvm::Value *value, const Type *type) {
  if (value && this->promiseValueSize(type)) {
    this->lowering.builder_.CreateStore(value, this->promiseValueAddress(promise));
  }
  this->lowering.builder_.CreateCall(this->lowering.asyncLowerer().asyncFunction(
                                         "tz_promise_fulfill", this->lowering.builder_.getVoidTy(),
                                         {this->lowering.builder_.getPtrTy()}),
                                     {promise});
}

void PromiseLowerer::followPromise(llvm::Value *promise, llvm::Value *other, uint32_t site) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->lowering.builder_.CreateCall(this->lowering.asyncLowerer().asyncFunction(
                                         "tz_promise_follow", this->lowering.builder_.getVoidTy(),
                                         {ptr, ptr, this->lowering.builder_.getInt32Ty()}),
                                     {promise, other, this->lowering.builder_.getInt32(site)});
}

llvm::Value *PromiseLowerer::promiseField(llvm::Value *promise, unsigned field) {
  return this->lowering.builder_.CreateStructGEP(this->lowering.promiseObjTy_, promise, field);
}

llvm::Constant *PromiseLowerer::resolverInfo(const Type *value) {
  llvm::Constant *&info = this->lowering.resolverInfos_[value];
  if (info) {
    return info;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  bool holds = this->promiseValueSize(value) != 0;
  std::vector<llvm::Type *> params{ptr};
  if (!value->isVoid()) {
    params.push_back(this->lowering.moduleBuilder().lower(value));
  }
  auto *code = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder_.getVoidTy(), params, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.resolve", this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
  llvm::Function *outer = std::exchange(this->lowering.function_, code);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", code));
  llvm::Value *promise = code->getArg(0);
  llvm::Value *locked = this->lowering.builder_.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_promise_lock",
                                                  this->lowering.builder_.getInt32Ty(), {ptr}),
      {promise}, "locked");
  llvm::BasicBlock *settleBlock = this->lowering.functionFrame().newBlock("settle");
  llvm::BasicBlock *dropBlock = this->lowering.functionFrame().newBlock("drop");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(locked), settleBlock,
                                       dropBlock);
  this->lowering.functionFrame().switchTo(settleBlock);
  this->fulfillPromise(promise, holds ? code->getArg(1) : nullptr, value);
  this->lowering.builder_.CreateRetVoid();
  this->lowering.functionFrame().switchTo(dropBlock);
  if (!value->isVoid()) {
    this->lowering.refCounts().releaseValue(code->getArg(1), value);
  }
  this->lowering.builder_.CreateRetVoid();
  this->lowering.function_ = outer;
  this->lowering.builder_.restoreIP(point);
  info = new llvm::GlobalVariable(
      *this->lowering.module_, this->lowering.fnInfoTy_, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(this->lowering.fnInfoTy_,
                                {code, this->lowering.stringLowerer().stringLiteral(u"")}),
      "tz.fninfo.resolve");

  return info;
}

llvm::Value *PromiseLowerer::emitPromiseNew(CallExpr *call) {
  const Type *value = call->type->promised();
  Expr *executorExpr = call->args[0];
  const Type *fnType = executorExpr->type;
  llvm::Value *executor = this->lowering.exprLowerer().emitExpr(executorExpr);
  llvm::Value *promise = this->newPromise(value);
  std::vector<llvm::Value *> args;
  if (!fnType->params().empty()) {
    this->lowering.builder_.CreateCall(this->lowering.objRetainFn_, {promise});
    args.push_back(
        this->lowering.closureLowerer().makeFunctionValue(this->resolverInfo(value), promise));
  }
  this->lowering.closureLowerer().callFunctionValue(executor, fnType, args);
  this->lowering.refCounts().releaseValue(executor, fnType);

  return promise;
}

llvm::Value *PromiseLowerer::emitPromiseStatic(CallExpr *call) {
  const Type *type = call->type;
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  switch (call->builtin()) {
    case Builtin::PromiseResolve: {
      if (!call->args.empty() && call->args[0]->type->isPromise()) {
        return this->lowering.exprLowerer().emitExpr(call->args[0]);
      }
      llvm::Value *value = nullptr;
      if (!call->args.empty()) {
        value = this->lowering.exprLowerer().emitExpr(call->args[0]);
      }
      auto *fn = cast<llvm::Function>(this->lowering.asyncLowerer()
                                          .asyncFunction("tz_promise_resolved", ptr, {ptr})
                                          .getCallee());
      llvm::Value *promise = this->lowering.builder_.CreateCall(
          fn, {this->promiseLayout(type->promised())}, "promise");
      if (value && this->promiseValueSize(type->promised())) {
        this->lowering.builder_.CreateStore(value, this->promiseValueAddress(promise));
      }
      return promise;
    }
    case Builtin::PromiseWithResolvers: {
      const Type *promiseType = type->field("promise")->type;
      const Type *value = promiseType->promised();
      llvm::Value *promise = this->newPromise(value);
      llvm::Value *object = this->lowering.objectLayouts().allocObject(type, {0, 1});
      this->lowering.builder_.CreateStore(
          promise, this->lowering.objectLayouts().fieldAddress(object, type, "promise"));
      this->lowering.builder_.CreateCall(this->lowering.objRetainFn_, {promise});
      this->lowering.builder_.CreateStore(
          this->lowering.closureLowerer().makeFunctionValue(this->resolverInfo(value), promise),
          this->lowering.objectLayouts().fieldAddress(object, type, "resolve"));
      this->lowering.groupLowerer().adoptFields(object, type);
      return object;
    }
    default:
      return this->lowering.promiseCombineLowerer().emitPromiseCombine(call);
  }
}

}
