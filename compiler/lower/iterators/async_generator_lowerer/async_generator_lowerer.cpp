#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;

namespace tz {

namespace {

constexpr uint64_t kAgenReactor = 40;

constexpr uint64_t kAgenHeader = 88;

constexpr uint32_t kAgenFrameAlign = 8;

constexpr uint32_t kLayoutAsyncGenerator = 268435456 | 536870912;

constexpr unsigned kPromiseFlags = 5;

constexpr uint32_t kPromiseDone = 8;

}

AsyncGeneratorLowerer::AsyncGeneratorLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Constant *AsyncGeneratorLowerer::asyncGeneratorLayout() {
  if (this->agenLayout) {
    return this->agenLayout;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  auto *drop = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder_.getVoidTy(), {ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.agen.destroy", this->lowering.module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx_, "entry", drop));
    b.CreateCall(
        this->lowering.asyncLowerer().asyncFunction("tz_agen_drop_steps", b.getVoidTy(), {ptr}),
        {drop->getArg(0)});
    llvm::Value *frame =
        b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), drop->getArg(0), kAgenHeader, "frame");
    b.CreateCall(this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_destroy),
                 {frame});
    b.CreateRetVoid();
  }
  auto callee = [&](llvm::StringRef name) {
    return cast<llvm::Constant>(
        this->lowering.asyncLowerer()
            .asyncFunction(name, this->lowering.builder_.getVoidTy(), {ptr, ptr})
            .getCallee());
  };
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  auto *type = llvm::StructType::get(this->lowering.ctx_, {this->lowering.layoutDescTy_, ptr, ptr});
  llvm::Constant *base = llvm::ConstantStruct::get(
      this->lowering.layoutDescTy_,
      {drop, null, this->lowering.builder_.getInt32(0),
       this->lowering.builder_.getInt32(kLayoutAsyncGenerator), null,
       this->lowering.builder_.getInt32(0), this->lowering.builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(
      *this->lowering.module_, type, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(type, {base, callee("tz_agen_next"), callee("tz_agen_close")}),
      "tz.layout.agen");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  this->agenLayout = global;

  return this->agenLayout;
}

void AsyncGeneratorLowerer::emitAsyncGenerator(FunctionDecl *fn, llvm::Function *function) {
  this->lowering.functionBuilder().beginFunction(function, fn);
  Lowering::Coroutine coroutine{};
  coroutine.header = kAgenHeader;
  this->lowering.coro_ = &coroutine;
  Lowering::AsyncState state{};
  state.generator = true;
  this->lowering.async_ = &state;
  this->lowering.function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  llvm::Type *i8 = this->lowering.builder_.getInt8Ty();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  coroutine.id = this->lowering.builder_.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_id),
      {this->lowering.builder_.getInt32(kAgenFrameAlign), null, null, null}, "id");
  llvm::Value *size = this->lowering.builder_.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {},
      "size");
  llvm::Value *memory = this->lowering.builder_.CreateCall(
      this->lowering.rt_.objAlloc,
      {this->lowering.builder_.CreateAdd(size, this->lowering.builder_.getInt64(kAgenHeader))},
      "memory");
  this->lowering.builder_.CreateMemSet(memory, this->lowering.builder_.getInt8(0), kAgenHeader,
                                       llvm::MaybeAlign(8));
  this->lowering.builder_.CreateStore(this->lowering.builder_.getInt64(1), memory);
  this->lowering.builder_.CreateStore(
      this->asyncGeneratorLayout(),
      this->lowering.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor =
      this->lowering.builder_.CreateConstInBoundsGEP1_64(i8, memory, kAgenReactor);
  this->lowering.builder_.CreateStore(
      this->lowering.asyncLowerer().asyncResumeFunction(),
      this->lowering.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  this->lowering.builder_.CreateStore(
      memory, this->lowering.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame =
      this->lowering.builder_.CreateConstInBoundsGEP1_64(i8, memory, kAgenHeader, "frame");
  coroutine.handle = this->lowering.builder_.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_begin),
      {coroutine.id, frame}, "handle");
  coroutine.object = this->lowering.builder_.CreateGEP(
      i8, coroutine.handle, llvm::ConstantInt::getSigned(i64, -int64_t(kAgenHeader)), "generator");
  state.object = coroutine.object;
  state.reactor = this->lowering.builder_.CreateConstInBoundsGEP1_64(i8, coroutine.object,
                                                                     kAgenReactor, "reactor");
  coroutine.finalBlock = this->lowering.functionFrame().newBlock("coro.final");
  coroutine.freeBlock = this->lowering.functionFrame().newBlock("coro.free");
  coroutine.returnBlock = this->lowering.functionFrame().newBlock("coro.return");

  this->lowering.functionFrame().pushScope();
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    this->lowering.envParam_ = this->lowering.function_->getArg(0);
    if (fn->isClosure()) {
      this->lowering.envLowerer().bindCaptures(fn);
    } else {
      this->lowering.envParam_->setName("this");
    }
    llvm::Value *slot = this->lowering.functionFrame().createObjectSlot("env");
    this->lowering.builder_.CreateCall(this->lowering.objRetainFn_, {this->lowering.envParam_});
    this->lowering.builder_.CreateStore(this->lowering.envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < this->lowering.function_->arg_size(); ++i) {
    args.push_back(this->lowering.function_->getArg(i));
  }
  this->lowering.functionBuilder().emitParams(fn, args);
  this->lowering.coroutineLowerer().emitSuspend();
  this->lowering.stmtLowerer().emitStmt(fn->body);
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->lowering.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
  }
  this->lowering.scopes_.pop_back();
  this->lowering.coroutineLowerer().finishCoroutine();
  this->lowering.coro_ = nullptr;
  this->lowering.async_ = nullptr;
  this->lowering.function_ = nullptr;
}

void AsyncGeneratorLowerer::finishAsyncGenerator() {
  this->lowering.builder_.CreateCall(this->lowering.asyncLowerer().asyncFunction(
                                         "tz_agen_finish", this->lowering.builder_.getVoidTy(),
                                         {this->lowering.builder_.getPtrTy()}),
                                     {this->lowering.async_->object});
}

void AsyncGeneratorLowerer::emitAsyncGeneratorReturn(ReturnStmt *stmt) {
  if (stmt->value) {
    const Type *type = stmt->value->type;
    llvm::Value *value =
        this->awaitValueOf(this->lowering.exprLowerer().emitExpr(stmt->value), type, stmt->loc());
    const Type *given = Checker::awaitedTypeOf(type);
    if (value && !given->isVoid()) {
      this->lowering.refCounts().releaseValue(value, given);
    }
  }
  this->lowering.exitLowerer().emitReturnOf(nullptr);
}

llvm::Value *AsyncGeneratorLowerer::awaitValueOf(llvm::Value *value, const Type *type,
                                                 llvm::SMLoc loc) {
  if (type->isPromise()) {
    return this->lowering.asyncLowerer().awaitPromise(value, type, loc);
  }
  if (AsyncLowerer::holdsPromise(type)) {
    return this->lowering.asyncLowerer().awaitUnion(value, type, Checker::awaitedTypeOf(type), loc);
  }
  this->lowering.asyncLowerer().suspendAsync(nullptr, loc);

  return value;
}

void AsyncGeneratorLowerer::agenYield(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Value *slot = value && this->lowering.promiseLowerer().promiseValueSize(type)
                          ? this->lowering.functionFrame().spill(value, "yielded")
                          : static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(ptr));
  if (value && !this->lowering.promiseLowerer().promiseValueSize(type)) {
    this->lowering.refCounts().releaseValue(value, type);
  }
  llvm::Value *descriptor =
      this->lowering.promiseLowerer().promiseValueSize(type)
          ? static_cast<llvm::Value *>(this->lowering.typeDescriptors().typeDescriptor(type))
          : llvm::ConstantPointerNull::get(ptr);
  llvm::Value *next = this->lowering.builder_.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_agen_yield", i32, {ptr, ptr, ptr}),
      {this->lowering.async_->object, slot, descriptor}, "next");
  llvm::BasicBlock *waitBlock = this->lowering.functionFrame().newBlock("yield.wait");
  llvm::BasicBlock *decideBlock = this->lowering.functionFrame().newBlock("yield.decide");
  llvm::BasicBlock *closeBlock = this->lowering.functionFrame().newBlock("yield.close");
  llvm::BasicBlock *goBlock = this->lowering.functionFrame().newBlock("yield.go");
  llvm::BasicBlock *before = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpSLT(next, this->lowering.builder_.getInt32(0)), waitBlock,
      decideBlock);
  this->lowering.functionFrame().switchTo(waitBlock);
  this->lowering.coroutineLowerer().emitSuspend();
  llvm::Value *resumed = this->lowering.builder_.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_agen_resumed", i32, {ptr}),
      {this->lowering.async_->object}, "resumed");
  llvm::BasicBlock *waited = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(decideBlock);
  this->lowering.functionFrame().switchTo(decideBlock);
  llvm::PHINode *how = this->lowering.builder_.CreatePHI(i32, 2, "how");
  how->addIncoming(next, before);
  how->addIncoming(resumed, waited);
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpEQ(how, this->lowering.builder_.getInt32(1)), closeBlock,
      goBlock, llvm::MDBuilder(this->lowering.ctx_).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(closeBlock);
  this->lowering.asyncLowerer().suspendAsync(nullptr, loc);
  this->lowering.exitLowerer().emitReturnOf(nullptr);
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->lowering.builder_.CreateUnreachable();
  }
  this->lowering.functionFrame().switchTo(goBlock);
}

llvm::Value *AsyncGeneratorLowerer::emitAsyncYield(YieldExpr *yield) {
  const Type *yielded = this->lowering.currentFn_->sym->ret->yielded();
  if (yield->delegate) {
    this->emitAsyncYieldAll(yield, yielded);
    return this->lowering.builder_.getFalse();
  }
  llvm::Value *value = nullptr;
  const Type *given = Type::undefinedTy();
  if (yield->value) {
    const Type *type = yield->value->type;
    value =
        this->awaitValueOf(this->lowering.exprLowerer().emitExpr(yield->value), type, yield->loc());
    given = Checker::awaitedTypeOf(type);
  } else {
    this->lowering.asyncLowerer().suspendAsync(nullptr, yield->loc());
  }
  llvm::Value *converted = nullptr;
  if (value && !given->isVoid() && !given->isUndefined()) {
    converted = this->lowering.exprLowerer().emitConversion(value, given, yielded);
  } else if (!yielded->isUndefined() && yielded->isOptional()) {
    converted = this->lowering.optionalLowerer().undefinedValue(yielded);
  }
  this->agenYield(converted, yielded, yield->loc());

  return this->lowering.builder_.getFalse();
}

llvm::Value *AsyncGeneratorLowerer::emitAsyncStep(llvm::Value *slot, const Type *element,
                                                  llvm::SMLoc loc, llvm::BasicBlock *endBlock) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Value *it = this->lowering.builder_.CreateLoad(ptr, slot, "iterator");
  llvm::Value *step = this->lowering.promiseLowerer().newPromise(element);
  this->lowering.builder_.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_async_iter_next",
                                                  this->lowering.builder_.getVoidTy(), {ptr, ptr}),
      {it, step});
  this->lowering.asyncLowerer().suspendAsync(step, loc);
  auto forget = [&] {
    llvm::Value *held = this->lowering.builder_.CreateLoad(ptr, slot, "iterator");
    this->lowering.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), slot);
    this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {held});
  };
  llvm::Value *flags = this->lowering.builder_.CreateLoad(
      i32, this->lowering.promiseLowerer().promiseField(step, kPromiseFlags), "flags");
  llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("for.await.done");
  llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("for.await.value");
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpNE(
          this->lowering.builder_.CreateAnd(flags, this->lowering.builder_.getInt32(kPromiseDone)),
          this->lowering.builder_.getInt32(0)),
      doneBlock, valueBlock);
  this->lowering.functionFrame().switchTo(doneBlock);
  this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {step});
  forget();
  this->lowering.builder_.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(valueBlock);
  llvm::Value *value;
  if (this->lowering.promiseLowerer().promiseValueSize(element)) {
    value = this->lowering.builder_.CreateLoad(
        this->lowering.moduleBuilder().lower(element),
        this->lowering.promiseLowerer().promiseValueAddress(step), "value");
    this->lowering.refCounts().retainValue(value, element);
  } else {
    value = element->isUndefined() ? static_cast<llvm::Value *>(this->lowering.builder_.getFalse())
                                   : this->lowering.optionalLowerer().undefinedValue(element);
  }
  this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {step});

  return value;
}

void AsyncGeneratorLowerer::closeAsyncIterator(llvm::Value *slot) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *it = this->lowering.builder_.CreateLoad(ptr, slot, "iterator");
  llvm::BasicBlock *closeBlock = this->lowering.functionFrame().newBlock("for.await.close");
  llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("for.await.closed");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(it), closeBlock,
                                       nextBlock);
  this->lowering.functionFrame().switchTo(closeBlock);
  if (this->lowering.cleanupNoAwait_ || !this->lowering.async_) {
    this->lowering.builder_.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(nextBlock);
    return;
  }
  llvm::Value *closed = this->lowering.promiseLowerer().newPromise(Type::voidTy());
  this->lowering.builder_.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_async_iter_close",
                                                  this->lowering.builder_.getVoidTy(), {ptr, ptr}),
      {it, closed});
  this->lowering.asyncLowerer().suspendAsync(closed, this->lowering.loc_);
  this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {closed});
  this->lowering.builder_.CreateBr(nextBlock);
  this->lowering.functionFrame().switchTo(nextBlock);
}

void AsyncGeneratorLowerer::emitAsyncYieldAll(YieldExpr *yield, const Type *yielded) {
  const Type *type = yield->value->type;
  const Type *element = yield->element;
  this->lowering.functionFrame().pushScope();
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot = this->lowering.functionFrame().createManagedSlot(type, "yield.iterator");
    this->lowering.builder_.CreateStore(this->lowering.exprLowerer().emitExpr(yield->value), slot);
    this->lowering.scopes_.back().asyncIterators.push_back(slot);
    llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("yield.all.next");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("yield.all.end");
    this->lowering.functionFrame().emitBlock(headBlock);
    llvm::Value *value = this->emitAsyncStep(slot, element, yield->loc(), endBlock);
    this->agenYield(this->lowering.exprLowerer().emitConversion(value, element, yielded), yielded,
                    yield->loc());
    this->lowering.builder_.CreateBr(headBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    this->lowering.functionFrame().popScope();
    return;
  }
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(yield->value);
  size_t depth = this->lowering.scopes_.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("yield.all.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  llvm::SMLoc loc = yield->loc();
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, from, element, yielded, loc](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = this->awaitValueOf(value, from, loc);
    this->lowering.asyncLowerer().suspendAsync(nullptr, loc);
    this->agenYield(this->lowering.exprLowerer().emitConversion(awaited, element, yielded), yielded,
                    loc);

    return nullptr;
  };
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  this->lowering.functionFrame().popScope();
}

void AsyncGeneratorLowerer::emitForAwait(ForOfStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(this->lowering.pendingLabels_);
  this->lowering.pendingLabels_.clear();
  const Type *type = stmt->iterable->type;
  const Type *element = stmt->binding.sym->type;
  this->lowering.functionFrame().pushScope();
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot =
        this->lowering.functionFrame().createManagedSlot(type, "for.await.iterator");
    this->lowering.builder_.CreateStore(this->lowering.exprLowerer().emitExpr(stmt->iterable),
                                        slot);
    this->lowering.scopes_.back().asyncIterators.push_back(slot);
    size_t depth = this->lowering.scopes_.size();
    llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("for.await.next");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("for.await.end");
    this->lowering.functionFrame().emitBlock(headBlock);
    llvm::Value *value = this->emitAsyncStep(slot, element, stmt->loc(), endBlock);
    this->lowering.functionFrame().pushScope();
    this->lowering.stmtLowerer().storeReplacing(
        this->lowering.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value,
        element);
    this->lowering.targets_.push_back({labels, true, false, endBlock, headBlock, depth});
    this->lowering.stmtLowerer().emitStmt(stmt->body);
    this->lowering.targets_.pop_back();
    this->lowering.functionFrame().popScope();
    this->lowering.functionFrame().branchTo(headBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    this->lowering.functionFrame().popScope();
    return;
  }
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(stmt->iterable);
  size_t depth = this->lowering.scopes_.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("for.await.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock, from,
                   element](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = this->awaitValueOf(value, from, stmt->loc());
    this->lowering.asyncLowerer().suspendAsync(nullptr, stmt->loc());
    const Type *given = Checker::awaitedTypeOf(from);
    llvm::Value *converted =
        element->isUndefined() ? this->lowering.builder_.getFalse()
        : given->isVoid() ? this->lowering.optionalLowerer().undefinedValue(element)
                          : this->lowering.exprLowerer().emitConversion(awaited, given, element);
    if (element->isUndefined() && awaited && !given->isVoid()) {
      this->lowering.refCounts().releaseValue(awaited, given);
    }
    llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("for.await.next");
    size_t here = this->lowering.scopes_.size();
    this->lowering.functionFrame().pushScope();
    this->lowering.stmtLowerer().storeReplacing(
        this->lowering.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name),
        converted, element);
    this->lowering.targets_.push_back({labels, true, false, endBlock, nextBlock, depth, here});
    this->lowering.stmtLowerer().emitStmt(stmt->body);
    this->lowering.targets_.pop_back();
    this->lowering.functionFrame().popScope();
    this->lowering.functionFrame().emitBlock(nextBlock);

    return nullptr;
  };
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  this->lowering.functionFrame().popScope();
}

}
