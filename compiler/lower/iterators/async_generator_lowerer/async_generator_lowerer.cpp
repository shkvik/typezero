#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_types.h"
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
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  auto *drop = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder.getVoidTy(), {ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.agen.destroy", this->lowering.module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx, "entry", drop));
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
            .asyncFunction(name, this->lowering.builder.getVoidTy(), {ptr, ptr})
            .getCallee());
  };
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  auto *type = llvm::StructType::get(this->lowering.ctx, {this->lowering.layoutDescTy, ptr, ptr});
  llvm::Constant *base = llvm::ConstantStruct::get(
      this->lowering.layoutDescTy,
      {drop, null, this->lowering.builder.getInt32(0),
       this->lowering.builder.getInt32(kLayoutAsyncGenerator), null,
       this->lowering.builder.getInt32(0), this->lowering.builder.getInt32(0)});
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
  this->lowering.coro = &coroutine;
  Lowering::AsyncState state{};
  state.generator = true;
  this->lowering.async = &state;
  this->lowering.function->addFnAttr(llvm::Attribute::PresplitCoroutine);
  llvm::Type *i8 = this->lowering.builder.getInt8Ty();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  coroutine.id = this->lowering.builder.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_id),
      {this->lowering.builder.getInt32(kAgenFrameAlign), null, null, null}, "id");
  llvm::Value *size = this->lowering.builder.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {},
      "size");
  llvm::Value *memory = this->lowering.builder.CreateCall(
      this->lowering.rt_.objAlloc,
      {this->lowering.builder.CreateAdd(size, this->lowering.builder.getInt64(kAgenHeader))},
      "memory");
  this->lowering.builder.CreateMemSet(memory, this->lowering.builder.getInt8(0), kAgenHeader,
                                      llvm::MaybeAlign(8));
  this->lowering.builder.CreateStore(this->lowering.builder.getInt64(1), memory);
  this->lowering.builder.CreateStore(
      this->asyncGeneratorLayout(),
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor =
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, memory, kAgenReactor);
  this->lowering.builder.CreateStore(
      this->lowering.asyncLowerer().asyncResumeFunction(),
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  this->lowering.builder.CreateStore(
      memory, this->lowering.builder.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame =
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, memory, kAgenHeader, "frame");
  coroutine.handle = this->lowering.builder.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_begin),
      {coroutine.id, frame}, "handle");
  coroutine.object = this->lowering.builder.CreateGEP(
      i8, coroutine.handle, llvm::ConstantInt::getSigned(i64, -int64_t(kAgenHeader)), "generator");
  state.object = coroutine.object;
  state.reactor = this->lowering.builder.CreateConstInBoundsGEP1_64(i8, coroutine.object,
                                                                    kAgenReactor, "reactor");
  coroutine.finalBlock = this->lowering.functionFrame().newBlock("coro.final");
  coroutine.freeBlock = this->lowering.functionFrame().newBlock("coro.free");
  coroutine.returnBlock = this->lowering.functionFrame().newBlock("coro.return");

  this->lowering.functionFrame().pushScope();
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    this->lowering.envParam = this->lowering.function->getArg(0);
    if (fn->isClosure()) {
      this->lowering.envLowerer().bindCaptures(fn);
    } else {
      this->lowering.envParam->setName("this");
    }
    llvm::Value *slot = this->lowering.functionFrame().createObjectSlot("env");
    this->lowering.builder.CreateCall(this->lowering.objRetainFn, {this->lowering.envParam});
    this->lowering.builder.CreateStore(this->lowering.envParam, slot);
    first = 1;
  }
  for (unsigned i = first; i < this->lowering.function->arg_size(); ++i) {
    args.push_back(this->lowering.function->getArg(i));
  }
  this->lowering.functionBuilder().emitParams(fn, args);
  this->lowering.coroutineLowerer().emitSuspend();
  this->lowering.stmtLowerer().emitStmt(fn->body);
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->lowering.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
  }
  this->lowering.scopes.pop_back();
  this->lowering.coroutineLowerer().finishCoroutine();
  this->lowering.coro = nullptr;
  this->lowering.async = nullptr;
  this->lowering.function = nullptr;
}

void AsyncGeneratorLowerer::finishAsyncGenerator() {
  this->lowering.builder.CreateCall(this->lowering.asyncLowerer().asyncFunction(
                                        "tz_agen_finish", this->lowering.builder.getVoidTy(),
                                        {this->lowering.builder.getPtrTy()}),
                                    {this->lowering.async->object});
}

void AsyncGeneratorLowerer::emitAsyncGeneratorReturn(ReturnStmt *stmt) {
  if (stmt->value) {
    const Type *type = stmt->value->type;
    llvm::Value *value =
        this->awaitValueOf(this->lowering.exprLowerer().emitExpr(stmt->value), type, stmt->loc());
    const Type *given = SemanticTypes::awaitedType(type);
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
    return this->lowering.asyncLowerer().awaitUnion(value, type, SemanticTypes::awaitedType(type),
                                                    loc);
  }
  this->lowering.asyncLowerer().suspendAsync(nullptr, loc);

  return value;
}

void AsyncGeneratorLowerer::agenYield(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
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
  llvm::Value *next = this->lowering.builder.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_agen_yield", i32, {ptr, ptr, ptr}),
      {this->lowering.async->object, slot, descriptor}, "next");
  llvm::BasicBlock *waitBlock = this->lowering.functionFrame().newBlock("yield.wait");
  llvm::BasicBlock *decideBlock = this->lowering.functionFrame().newBlock("yield.decide");
  llvm::BasicBlock *closeBlock = this->lowering.functionFrame().newBlock("yield.close");
  llvm::BasicBlock *goBlock = this->lowering.functionFrame().newBlock("yield.go");
  llvm::BasicBlock *before = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateCondBr(
      this->lowering.builder.CreateICmpSLT(next, this->lowering.builder.getInt32(0)), waitBlock,
      decideBlock);
  this->lowering.functionFrame().switchTo(waitBlock);
  this->lowering.coroutineLowerer().emitSuspend();
  llvm::Value *resumed = this->lowering.builder.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_agen_resumed", i32, {ptr}),
      {this->lowering.async->object}, "resumed");
  llvm::BasicBlock *waited = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(decideBlock);
  this->lowering.functionFrame().switchTo(decideBlock);
  llvm::PHINode *how = this->lowering.builder.CreatePHI(i32, 2, "how");
  how->addIncoming(next, before);
  how->addIncoming(resumed, waited);
  this->lowering.builder.CreateCondBr(
      this->lowering.builder.CreateICmpEQ(how, this->lowering.builder.getInt32(1)), closeBlock,
      goBlock, llvm::MDBuilder(this->lowering.ctx).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(closeBlock);
  this->lowering.asyncLowerer().suspendAsync(nullptr, loc);
  this->lowering.exitLowerer().emitReturnOf(nullptr);
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->lowering.builder.CreateUnreachable();
  }
  this->lowering.functionFrame().switchTo(goBlock);
}

llvm::Value *AsyncGeneratorLowerer::emitAsyncYield(YieldExpr *yield) {
  const Type *yielded = this->lowering.currentFn->sym->getKnownReturnType()->yielded();
  if (yield->delegate) {
    this->emitAsyncYieldAll(yield, yielded);
    return this->lowering.builder.getFalse();
  }
  llvm::Value *value = nullptr;
  const Type *given = Type::undefinedTy();
  if (yield->value) {
    const Type *type = yield->value->type;
    value =
        this->awaitValueOf(this->lowering.exprLowerer().emitExpr(yield->value), type, yield->loc());
    given = SemanticTypes::awaitedType(type);
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

  return this->lowering.builder.getFalse();
}

llvm::Value *AsyncGeneratorLowerer::emitAsyncStep(llvm::Value *slot, const Type *element,
                                                  llvm::SMLoc loc, llvm::BasicBlock *endBlock) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Value *it = this->lowering.builder.CreateLoad(ptr, slot, "iterator");
  llvm::Value *step = this->lowering.promiseLowerer().newPromise(element);
  this->lowering.builder.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_async_iter_next",
                                                  this->lowering.builder.getVoidTy(), {ptr, ptr}),
      {it, step});
  this->lowering.asyncLowerer().suspendAsync(step, loc);
  auto forget = [&] {
    llvm::Value *held = this->lowering.builder.CreateLoad(ptr, slot, "iterator");
    this->lowering.builder.CreateStore(llvm::ConstantPointerNull::get(ptr), slot);
    this->lowering.builder.CreateCall(this->lowering.instanceReleaseFn, {held});
  };
  llvm::Value *flags = this->lowering.builder.CreateLoad(
      i32, this->lowering.promiseLowerer().promiseField(step, kPromiseFlags), "flags");
  llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("for.await.done");
  llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("for.await.value");
  this->lowering.builder.CreateCondBr(
      this->lowering.builder.CreateICmpNE(
          this->lowering.builder.CreateAnd(flags, this->lowering.builder.getInt32(kPromiseDone)),
          this->lowering.builder.getInt32(0)),
      doneBlock, valueBlock);
  this->lowering.functionFrame().switchTo(doneBlock);
  this->lowering.builder.CreateCall(this->lowering.instanceReleaseFn, {step});
  forget();
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(valueBlock);
  llvm::Value *value;
  if (this->lowering.promiseLowerer().promiseValueSize(element)) {
    value = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(element),
        this->lowering.promiseLowerer().promiseValueAddress(step), "value");
    this->lowering.refCounts().retainValue(value, element);
  } else {
    value = element->isUndefined() ? static_cast<llvm::Value *>(this->lowering.builder.getFalse())
                                   : this->lowering.optionalLowerer().undefinedValue(element);
  }
  this->lowering.builder.CreateCall(this->lowering.instanceReleaseFn, {step});

  return value;
}

void AsyncGeneratorLowerer::closeAsyncIterator(llvm::Value *slot) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Value *it = this->lowering.builder.CreateLoad(ptr, slot, "iterator");
  llvm::BasicBlock *closeBlock = this->lowering.functionFrame().newBlock("for.await.close");
  llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("for.await.closed");
  this->lowering.builder.CreateCondBr(this->lowering.builder.CreateIsNotNull(it), closeBlock,
                                      nextBlock);
  this->lowering.functionFrame().switchTo(closeBlock);
  if (this->lowering.cleanupNoAwait || !this->lowering.async) {
    this->lowering.builder.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(nextBlock);
    return;
  }
  llvm::Value *closed = this->lowering.promiseLowerer().newPromise(Type::voidTy());
  this->lowering.builder.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_async_iter_close",
                                                  this->lowering.builder.getVoidTy(), {ptr, ptr}),
      {it, closed});
  this->lowering.asyncLowerer().suspendAsync(closed, this->lowering.loc_);
  this->lowering.builder.CreateCall(this->lowering.instanceReleaseFn, {closed});
  this->lowering.builder.CreateBr(nextBlock);
  this->lowering.functionFrame().switchTo(nextBlock);
}

void AsyncGeneratorLowerer::emitAsyncYieldAll(YieldExpr *yield, const Type *yielded) {
  const Type *type = yield->value->type;
  const Type *element = yield->element;
  this->lowering.functionFrame().pushScope();
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot = this->lowering.functionFrame().createManagedSlot(type, "yield.iterator");
    this->lowering.builder.CreateStore(this->lowering.exprLowerer().emitExpr(yield->value), slot);
    this->lowering.scopes.back().asyncIterators.push_back(slot);
    llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("yield.all.next");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("yield.all.end");
    this->lowering.functionFrame().emitBlock(headBlock);
    llvm::Value *value = this->emitAsyncStep(slot, element, yield->loc(), endBlock);
    this->agenYield(this->lowering.exprLowerer().emitConversion(value, element, yielded), yielded,
                    yield->loc());
    this->lowering.builder.CreateBr(headBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    this->lowering.functionFrame().popScope();
    return;
  }
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(yield->value);
  size_t depth = this->lowering.scopes.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("yield.all.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  llvm::SMLoc loc = yield->loc();
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder.CreateBr(endBlock);
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
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(this->lowering.pendingLabels);
  this->lowering.pendingLabels.clear();
  const Type *type = stmt->iterable->type;
  const Type *element = stmt->binding.sym->type;
  this->lowering.functionFrame().pushScope();
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot =
        this->lowering.functionFrame().createManagedSlot(type, "for.await.iterator");
    this->lowering.builder.CreateStore(this->lowering.exprLowerer().emitExpr(stmt->iterable), slot);
    this->lowering.scopes.back().asyncIterators.push_back(slot);
    size_t depth = this->lowering.scopes.size();
    llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("for.await.next");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("for.await.end");
    this->lowering.functionFrame().emitBlock(headBlock);
    llvm::Value *value = this->emitAsyncStep(slot, element, stmt->loc(), endBlock);
    this->lowering.functionFrame().pushScope();
    this->lowering.stmtLowerer().storeReplacing(
        this->lowering.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value,
        element);
    this->lowering.targets.push_back({labels, true, false, endBlock, headBlock, depth});
    this->lowering.stmtLowerer().emitStmt(stmt->body);
    this->lowering.targets.pop_back();
    this->lowering.functionFrame().popScope();
    this->lowering.functionFrame().branchTo(headBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    this->lowering.functionFrame().popScope();
    return;
  }
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(stmt->iterable);
  size_t depth = this->lowering.scopes.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("for.await.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock, from,
                   element](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = this->awaitValueOf(value, from, stmt->loc());
    this->lowering.asyncLowerer().suspendAsync(nullptr, stmt->loc());
    const Type *given = SemanticTypes::awaitedType(from);
    llvm::Value *converted =
        element->isUndefined() ? this->lowering.builder.getFalse()
        : given->isVoid() ? this->lowering.optionalLowerer().undefinedValue(element)
                          : this->lowering.exprLowerer().emitConversion(awaited, given, element);
    if (element->isUndefined() && awaited && !given->isVoid()) {
      this->lowering.refCounts().releaseValue(awaited, given);
    }
    llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("for.await.next");
    size_t here = this->lowering.scopes.size();
    this->lowering.functionFrame().pushScope();
    this->lowering.stmtLowerer().storeReplacing(
        this->lowering.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name),
        converted, element);
    this->lowering.targets.push_back({labels, true, false, endBlock, nextBlock, depth, here});
    this->lowering.stmtLowerer().emitStmt(stmt->body);
    this->lowering.targets.pop_back();
    this->lowering.functionFrame().popScope();
    this->lowering.functionFrame().emitBlock(nextBlock);

    return nullptr;
  };
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  this->lowering.functionFrame().popScope();
}

}
