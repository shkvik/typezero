#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/debug_info/debug_info.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/places/place_lowerer/place_lowerer.h"
#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr uint64_t kPromiseHeader = 40;

constexpr uint64_t kReactorSize = 48;

constexpr uint64_t kReactorContext = 40;

constexpr uint32_t kAsyncFrameAlign = 8;

}

const Expr *AsyncLowerer::resolvedValue(const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  if (!call || call->builtin() != Builtin::PromiseResolve || call->args.size() != 1 ||
      call->args[0]->type->isPromise()) {
    return nullptr;
  }

  return call->args[0];
}

AsyncLowerer::AsyncLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::FunctionCallee AsyncLowerer::asyncFunction(llvm::StringRef name, llvm::Type *ret,
                                                 llvm::ArrayRef<llvm::Type *> params) {
  return this->lowering.processLowerer().processFunction(name, ret, params);
}

llvm::Function *AsyncLowerer::asyncResumeFunction() {
  if (this->asyncResumeFn) {
    return this->asyncResumeFn;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  this->asyncResumeFn = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.async.resume", this->lowering.module_.get());
  this->asyncResumeFn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx, "entry", this->asyncResumeFn));
  llvm::Value *reactor = this->asyncResumeFn->getArg(0);
  llvm::Value *frame = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), reactor, kReactorSize, "frame");
  llvm::Function *resume =
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_resume);
  if (!this->lowering.contexts()) {
    b.CreateCall(resume, {frame});
    b.CreateRetVoid();
    return this->asyncResumeFn;
  }
  llvm::Value *slot =
      b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), reactor, kReactorContext, "context.slot");
  llvm::Value *context = b.CreateLoad(ptr, slot, "context");
  auto *plain = llvm::BasicBlock::Create(this->lowering.ctx, "plain", this->asyncResumeFn);
  auto *within = llvm::BasicBlock::Create(this->lowering.ctx, "within", this->asyncResumeFn);
  b.CreateCondBr(b.CreateIsNull(context), plain, within);
  b.SetInsertPoint(plain);
  b.CreateCall(resume, {frame});
  b.CreateRetVoid();
  b.SetInsertPoint(within);
  b.CreateStore(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), slot);
  llvm::Value *outer =
      b.CreateCall(this->asyncFunction("tz_context_resume", ptr, {ptr}), {context}, "outer");
  b.CreateCall(resume, {frame});
  b.CreateCall(this->asyncFunction("tz_context_done", b.getVoidTy(), {ptr, ptr}), {outer, context});
  b.CreateRetVoid();

  return this->asyncResumeFn;
}

void AsyncLowerer::beginAsync() {
  this->lowering.function->addFnAttr(llvm::Attribute::PresplitCoroutine);
  Lowering::Coroutine &c = *this->lowering.coro;
  Lowering::AsyncState &a = *this->lowering.async;
  llvm::Type *i8 = this->lowering.builder.getInt8Ty();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  uint64_t reactorOffset =
      kPromiseHeader + this->lowering.promiseLowerer().promiseValueSize(a.value);
  uint64_t frameOffset = reactorOffset + kReactorSize;
  c.id = this->lowering.builder.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_id),
      {this->lowering.builder.getInt32(kAsyncFrameAlign), null, null, null}, "id");
  llvm::Value *size = this->lowering.builder.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {},
      "size");
  bool member = this->lowering.memory().promiseIsMember(a.value);
  llvm::Value *total =
      this->lowering.builder.CreateAdd(size, this->lowering.builder.getInt64(frameOffset));
  llvm::Value *memory =
      member ? this->lowering.builder.CreateCall(
                   this->lowering.groupLowerer().groupFunction(
                       "tz_group_alloc", this->lowering.builder.getPtrTy(), {i64}),
                   {total}, "memory")
             : this->lowering.builder.CreateCall(this->lowering.rt_.objAlloc, {total}, "memory");
  this->lowering.builder.CreateMemSet(
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, memory, 8),
      this->lowering.builder.getInt8(0), frameOffset - 8, llvm::MaybeAlign(8));
  if (!member) {
    this->lowering.builder.CreateStore(this->lowering.builder.getInt64(1), memory);
  }
  this->lowering.builder.CreateStore(
      this->lowering.promiseLowerer().promiseLayout(a.value),
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor =
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, memory, reactorOffset);
  this->lowering.builder.CreateStore(
      this->asyncResumeFunction(),
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  this->lowering.builder.CreateStore(
      memory, this->lowering.builder.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame =
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, memory, frameOffset, "frame");
  c.handle = this->lowering.builder.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_begin), {c.id, frame},
      "handle");
  c.object = this->lowering.builder.CreateGEP(
      i8, c.handle, llvm::ConstantInt::getSigned(i64, -int64_t(frameOffset)), "promise");
  a.object = c.object;
  a.reactor =
      this->lowering.builder.CreateConstInBoundsGEP1_64(i8, c.object, reactorOffset, "reactor");
  c.finalBlock = this->lowering.functionFrame().newBlock("coro.final");
  c.freeBlock = this->lowering.functionFrame().newBlock("coro.free");
  c.returnBlock = this->lowering.functionFrame().newBlock("coro.return");
}

void AsyncLowerer::finishAsync() {
  Lowering::Coroutine &c = *this->lowering.coro;
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->lowering.builder.CreateUnreachable();
  }
  llvm::Value *none = llvm::ConstantTokenNone::get(this->lowering.ctx);
  this->lowering.functionFrame().switchTo(c.finalBlock);
  llvm::Value *state = this->lowering.builder.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_suspend),
      {none, this->lowering.builder.getTrue()}, "final");
  llvm::BasicBlock *resumedBlock = this->lowering.functionFrame().newBlock("coro.resumed.done");
  llvm::SwitchInst *next = this->lowering.builder.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(this->lowering.builder.getInt8(0), resumedBlock);
  next->addCase(this->lowering.builder.getInt8(1), c.freeBlock);
  this->lowering.functionFrame().switchTo(resumedBlock);
  this->lowering.builder.CreateUnreachable();
  this->lowering.functionFrame().switchTo(c.freeBlock);
  this->lowering.builder.CreateBr(c.returnBlock);
  this->lowering.functionFrame().switchTo(c.returnBlock);
  this->lowering.builder.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_end),
      {c.handle, this->lowering.builder.getFalse(), none});
  if (this->lowering.async->dropsPromise) {
    this->lowering.builder.CreateCall(this->lowering.instanceReleaseFn, {c.object});
    this->lowering.builder.CreateRetVoid();
  } else {
    this->lowering.builder.CreateRet(c.object);
  }
}

void AsyncLowerer::emitAsyncFunction(FunctionDecl *fn, llvm::Function *function) {
  this->lowering.functionBuilder().beginFunction(function, fn);
  Lowering::Coroutine coroutine{};
  this->lowering.coro = &coroutine;
  Lowering::AsyncState state{};
  this->lowering.async = &state;
  const Type *ret = fn->sym->getKnownReturnType();
  state.value = ret->isPromise() ? ret->promised() : Type::voidTy();
  state.dropsPromise = !ret->isPromise();
  this->beginAsync();
  this->lowering.functionFrame().pushScope();
  bool publishes = this->lowering.publishesFrame(fn);
  llvm::Value *awaited = nullptr;
  if (publishes) {
    awaited = this->lowering.builder.CreateCall(
        this->lowering.processLowerer().processFunction("tz_take_awaited",
                                                        this->lowering.builder.getInt32Ty(), {}),
        {}, "awaited");
  }
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
  if (publishes) {
    llvm::FunctionCallee publishObject = this->lowering.processLowerer().processFunction(
        "tz_publish_object", this->lowering.builder.getVoidTy(),
        {this->lowering.builder.getPtrTy()});
    if (fn->isClosure()) {
      this->lowering.builder.CreateCall(publishObject, {this->lowering.envParam});
    }
    llvm::BasicBlock *publishBlock = this->lowering.functionFrame().newBlock("publish");
    llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("publish.done");
    llvm::Value *shared =
        this->lowering.builder.CreateICmpEQ(awaited, this->lowering.builder.getInt32(0));
    this->lowering.builder.CreateCondBr(shared, publishBlock, doneBlock);
    this->lowering.functionFrame().switchTo(publishBlock);
    if (fn->takesThis() && !fn->isClosure()) {
      this->lowering.builder.CreateCall(publishObject, {this->lowering.envParam});
    }
    for (size_t i = 0; i < fn->params.size(); ++i) {
      const Param &param = fn->params[i];
      if (this->lowering.ownedParams.contains(param.sym)) {
        continue;
      }
      this->lowering.storeLowerer().publishValue(
          args[i], param.defaultValue() ? Type::optional(param.sym->type) : param.sym->type);
    }
    this->lowering.builder.CreateBr(doneBlock);
    this->lowering.functionFrame().switchTo(doneBlock);
  }
  this->lowering.stmtLowerer().emitStmt(fn->body);
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->emitAsyncReturn(nullptr);
  }
  this->lowering.scopes.pop_back();
  this->finishAsync();
  this->lowering.coro = nullptr;
  this->lowering.async = nullptr;
  this->lowering.function = nullptr;
}

llvm::Function *AsyncLowerer::emitAsyncMain(SourceFile &file) {
  auto *body = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder.getPtrTy(), false),
      llvm::GlobalValue::InternalLinkage, "tz.main.async", this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(body);
  this->lowering.functionBuilder().beginFunction(body, nullptr);
  this->lowering.debugInfo().beginTopLevel(body);
  Lowering::Coroutine coroutine{};
  this->lowering.coro = &coroutine;
  Lowering::AsyncState state{};
  this->lowering.async = &state;
  state.value = Type::voidTy();
  this->beginAsync();
  this->lowering.functionFrame().pushScope();
  for (Stmt *item : file.items) {
    if (!isa<FunctionDecl>(item)) {
      this->lowering.stmtLowerer().emitStmt(item);
    }
  }
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->emitAsyncReturn(nullptr);
  }
  this->lowering.scopes.pop_back();
  this->finishAsync();
  this->lowering.coro = nullptr;
  this->lowering.async = nullptr;
  this->lowering.function = nullptr;

  return body;
}

void AsyncLowerer::emitAsyncReturn(llvm::Value *value) {
  Lowering::AsyncState &a = *this->lowering.async;
  llvm::Value *object = a.object;
  if (value && !this->lowering.promiseLowerer().promiseValueSize(a.value)) {
    value = nullptr;
  }
  if (value) {
    this->lowering.builder.CreateStore(value,
                                       this->lowering.promiseLowerer().promiseValueAddress(object));
  }
  this->lowering.functionFrame().emitCleanups(0);
  this->lowering.promiseLowerer().fulfillPromise(object, nullptr, a.value);
  this->lowering.builder.CreateBr(this->lowering.coro->finalBlock);
  this->lowering.functionFrame().startDeadBlock();
}

void AsyncLowerer::emitAsyncReturnStmt(ReturnStmt *stmt) {
  Expr *value = stmt->value;
  if (!value) {
    this->lowering.exitLowerer().emitReturnOf(nullptr);
    return;
  }
  if (value->type->isPromise()) {
    llvm::Value *other = this->lowering.exprLowerer().emitExpr(value);
    this->lowering.functionFrame().emitCleanups(0);
    uint32_t site = 0;
    if (this->lowering.parallel_) {
      site = this->lowering.aheadStarts
                 .try_emplace(stmt, uint32_t(this->lowering.aheadStarts.size() + 1))
                 .first->second;
    }
    this->lowering.promiseLowerer().followPromise(this->lowering.async->object, other, site);
    this->lowering.builder.CreateBr(this->lowering.coro->finalBlock);
    this->lowering.functionFrame().startDeadBlock();
    return;
  }
  llvm::Value *result = this->lowering.exprLowerer().emitExpr(value);
  if (!this->lowering.promiseLowerer().promiseValueSize(this->lowering.async->value) &&
      !value->type->isVoid()) {
    this->lowering.refCounts().releaseValue(result, value->type);
    result = nullptr;
  }
  this->lowering.exitLowerer().emitReturnOf(value->type->isVoid() ? nullptr : result);
}

void AsyncLowerer::suspendAsync(llvm::Value *promise, llvm::SMLoc loc) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  uint32_t site = this->awaiting ? this->lowering.aheadStarts.lookup(this->awaiting) : 0;
  if (site && this->lowering.movableAwaits.contains(this->awaiting)) {
    site |= 1u << 18;
  }
  if (this->lowering.contexts()) {
    llvm::Value *context = this->lowering.builder.CreateCall(
        this->asyncFunction("tz_context_hold", ptr, {}), {}, "context");
    this->lowering.builder.CreateStore(
        context,
        this->lowering.builder.CreateConstInBoundsGEP1_64(
            this->lowering.builder.getInt8Ty(), this->lowering.async->reactor, kReactorContext));
  }
  llvm::Value *save = this->lowering.coroutineLowerer().emitSave();
  this->lowering.builder.CreateCall(
      this->asyncFunction("tz_async_await", this->lowering.builder.getVoidTy(),
                          {ptr, ptr, this->lowering.builder.getInt32Ty()}),
      {this->lowering.async->reactor, promise ? promise : llvm::ConstantPointerNull::get(ptr),
       this->lowering.builder.getInt32(site)});
  if (!this->lowering.currentFn) {
    auto *where =
        cast<llvm::GlobalVariable>(this->lowering.module_->getOrInsertGlobal("tz_main_await", ptr));
    this->lowering.builder.CreateStore(this->lowering.placeLowerer().frameDescriptor(loc), where);
  }
  this->lowering.coroutineLowerer().emitSuspend(save);
}

llvm::Value *AsyncLowerer::awaitPromise(llvm::Value *promise, const Type *type, llvm::SMLoc loc) {
  const Type *value = type->promised();
  this->suspendAsync(promise, loc);
  llvm::Value *result = nullptr;
  if (this->lowering.promiseLowerer().promiseValueSize(value)) {
    result = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(value),
        this->lowering.promiseLowerer().promiseValueAddress(promise), "awaited");
    this->lowering.refCounts().retainValue(result, value);
  }
  this->lowering.refCounts().releaseValue(promise, type);

  return result;
}

llvm::Value *AsyncLowerer::emitAwait(AwaitExpr *await) {
  Expr *operand = await->value;
  bool ahead = false;
  if (auto *call = dyn_cast<CallExpr>(operand);
      call &&
      (call->builtin() == Builtin::LibAhead || call->builtin() == Builtin::LibAheadMovable)) {
    if (this->lowering.parallel_) {
      this->lowering.aheadStarts.try_emplace(await,
                                             uint32_t(this->lowering.aheadStarts.size() + 1));
      if (call->builtin() == Builtin::LibAheadMovable) {
        this->lowering.movableAwaits.insert(await);
      }
    }
    operand = call->args[0];
    ahead = true;
  }
  const Expr *inner = AsyncLowerer::resolvedValue(operand);
  if (inner) {
    operand = const_cast<Expr *>(inner);
  }
  this->lowering.noteAwaited(operand);
  const Type *type = operand->type;
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(operand);
  const AwaitExpr *outer = std::exchange(this->awaiting, await);
  llvm::Value *result;
  if (type->isPromise()) {
    result = this->awaitPromise(value, type, await->loc());
  } else if (type->isUnknown()) {
    llvm::Type *ptr = this->lowering.builder.getPtrTy();
    llvm::Value *promise = this->lowering.builder.CreateCall(
        this->asyncFunction("tz_unknown_await", ptr, {ptr, ptr}),
        {value, this->lowering.promiseLowerer().promiseLayout(type)}, "unknown.promise");
    this->lowering.refCounts().releaseValue(value, type);
    result = this->awaitPromise(promise, Type::promise(type), await->loc());
  } else if (AsyncLowerer::holdsPromise(type)) {
    bool outerReady =
        std::exchange(this->readyGoesOn, ahead || this->lowering.freeAwaits.contains(await));
    result = this->awaitUnion(value, type, await->type, await->loc());
    this->readyGoesOn = outerReady;
  } else {
    this->suspendAsync(nullptr, await->loc());
    result = type->isVoid() ? nullptr : value;
  }
  this->awaiting = outer;

  return result;
}

bool AsyncLowerer::holdsPromise(const Type *type) {
  type = type->nonOptional();
  if (type->isPromise()) {
    return true;
  }

  return type->isUnion() &&
         llvm::any_of(type->members(), [](const Type *m) { return m->isPromise(); });
}

llvm::Value *AsyncLowerer::awaitUnion(llvm::Value *value, const Type *type, const Type *result,
                                      llvm::SMLoc loc) {
  llvm::Type *resultType =
      result->isVoid() ? nullptr : this->lowering.moduleBuilder().lower(result);
  auto plain = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    if (!this->readyGoesOn) {
      this->suspendAsync(nullptr, loc);
    }
    if (!resultType) {
      if (v) {
        this->lowering.refCounts().releaseValue(v, t);
      }
      return nullptr;
    }

    return v ? this->lowering.exprLowerer().emitConversion(v, t, result)
             : this->lowering.optionalLowerer().undefinedValue(result);
  };
  auto promise = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    llvm::Value *got = this->awaitPromise(v, t, loc);
    if (!resultType) {
      if (got) {
        this->lowering.refCounts().releaseValue(got, t->promised());
      }
      return nullptr;
    }

    return this->lowering.exprLowerer().emitConversion(got, t->promised(), result);
  };
  auto part = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    if (t->isPromise()) {
      return promise(v, t);
    }
    if (AsyncLowerer::holdsPromise(t)) {
      return this->awaitUnion(v, t, result, loc);
    }

    return plain(v, t);
  };
  if (type->isTagged()) {
    return this->lowering.unionLayout().forEachSlot(
        value, type->slots(),
        [&](UnionSlot slot) {
          return part(this->lowering.unionLayout().taggedPart(value, type, slot),
                      type->slotPart(slot));
        },
        resultType, resultType ? llvm::Constant::getNullValue(resultType) : nullptr);
  }
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("await.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  auto finish = [&](llvm::Value *v) {
    incoming.push_back({v, this->lowering.builder.GetInsertBlock()});
    this->lowering.builder.CreateBr(endBlock);
  };
  llvm::BasicBlock *promiseBlock = this->lowering.functionFrame().newBlock("await.promise");
  llvm::BasicBlock *otherBlock = this->lowering.functionFrame().newBlock("await.other");
  if (type->isOptional()) {
    this->lowering.builder.CreateCondBr(this->lowering.optionalLowerer().optionalHas(value, type),
                                        promiseBlock, otherBlock);
    this->lowering.functionFrame().switchTo(otherBlock);
    finish(plain(nullptr, type));
    this->lowering.functionFrame().switchTo(promiseBlock);
    finish(part(this->lowering.optionalLowerer().optionalValue(value, type), type->inner()));
  } else {
    const Type *promiseType =
        *llvm::find_if(type->members(), [](const Type *m) { return m->isPromise(); });
    this->lowering.builder.CreateCondBr(
        this->lowering.unionLowerer().emitIsOneOf(
            this->lowering.unionLowerer().objectClassId(value), promiseType),
        promiseBlock, otherBlock);
    this->lowering.functionFrame().switchTo(promiseBlock);
    finish(promise(value, promiseType));
    this->lowering.functionFrame().switchTo(otherBlock);
    finish(plain(value, UnionSlots::unionWithout(type, promiseType)));
  }
  this->lowering.functionFrame().switchTo(endBlock);
  if (!resultType) {
    return nullptr;
  }
  llvm::PHINode *phi = this->lowering.builder.CreatePHI(resultType, unsigned(incoming.size()));
  for (auto [v, block] : incoming) {
    phi->addIncoming(v, block);
  }

  return phi;
}

}
