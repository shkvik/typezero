#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/lower/errors/try_lowerer/try_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
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

constexpr uint64_t kReactorSize = 40;

enum PromiseField : unsigned { kRc, kLayout, kHead, kTail, kError, kState, kFlags };

constexpr uint32_t kFulfilled = 1;

// The alignment of a coroutine's frame, as for generators.
constexpr uint32_t kAsyncFrameAlign = 8;

// `Promise.resolve(x)` of a value that is no Promise.
const Expr *resolvedValue(const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  if (!call || call->builtin != Builtin::PromiseResolve || call->args.size() != 1 ||
      call->args[0]->type->isPromise()) {
    return nullptr;
  }

  return call->args[0];
}

} // namespace

AsyncLowerer::AsyncLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::FunctionCallee AsyncLowerer::asyncFunction(llvm::StringRef name, llvm::Type *ret,
                                            llvm::ArrayRef<llvm::Type *> params) {
  return codegen_.processLowerer().processFunction(name, ret, params);
}

// void (ptr reactor, ptr source): what the runtime calls to go on with an
// async function, whose frame follows the reactor.
llvm::Function *AsyncLowerer::asyncResumeFunction() {
  if (asyncResumeFn_) {
    return asyncResumeFn_;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  asyncResumeFn_ =
      llvm::Function::Create(llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr, ptr}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.async.resume", codegen_.module_.get());
  asyncResumeFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", asyncResumeFn_));
  llvm::Value *frame =
      b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), asyncResumeFn_->getArg(0), kReactorSize, "frame");
  b.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  b.CreateRetVoid();

  return asyncResumeFn_;
}

// The start of an async function: the promise, the reactor and the frame
// in one allocation, which the ramp gives back.
void AsyncLowerer::beginAsync() {
  codegen_.function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  CodeGen::Coroutine &c = *codegen_.coro_;
  CodeGen::AsyncState &a = *codegen_.async_;
  llvm::Type *i8 = codegen_.builder_.getInt8Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  uint64_t reactorOffset = kPromiseHeader + codegen_.promiseLowerer().promiseValueSize(a.value);
  uint64_t frameOffset = reactorOffset + kReactorSize;
  c.id = codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_id),
                             {codegen_.builder_.getInt32(kAsyncFrameAlign), null, null, null}, "id");
  llvm::Value *size =
      codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {}, "size");
  // A promise that may hold a member is one, with its frame (see
  // promiseIsMember); its count is set already.
  bool member = codegen_.promiseIsMember(a.value);
  llvm::Value *total = codegen_.builder_.CreateAdd(size, codegen_.builder_.getInt64(frameOffset));
  llvm::Value *memory =
      member ? codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_alloc", codegen_.builder_.getPtrTy(), {i64}),
                                   {total}, "memory")
             : codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {total}, "memory");
  codegen_.builder_.CreateMemSet(codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8), codegen_.builder_.getInt8(0),
                        frameOffset - 8, llvm::MaybeAlign(8));
  if (!member) {
    codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), memory);
  }
  codegen_.builder_.CreateStore(codegen_.promiseLowerer().promiseLayout(a.value), codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor = codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, reactorOffset);
  codegen_.builder_.CreateStore(asyncResumeFunction(), codegen_.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  codegen_.builder_.CreateStore(memory, codegen_.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame = codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, frameOffset, "frame");
  c.handle =
      codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_begin), {c.id, frame}, "handle");
  // As for a generator, the object comes from the handle: LLVM takes the
  // frame to be reached only through it.
  c.object = codegen_.builder_.CreateGEP(
      i8, c.handle, llvm::ConstantInt::getSigned(i64, -int64_t(frameOffset)), "promise");
  a.object = c.object;
  a.reactor = codegen_.builder_.CreateConstInBoundsGEP1_64(i8, c.object, reactorOffset, "reactor");
  c.finalBlock = codegen_.functionFrame().newBlock("coro.final");
  c.freeBlock = codegen_.functionFrame().newBlock("coro.free");
  c.returnBlock = codegen_.functionFrame().newBlock("coro.return");
}

// The final suspension. The memory is the promise's, which its last release
// frees: nothing destroys the coroutine.
void AsyncLowerer::finishAsync() {
  CodeGen::Coroutine &c = *codegen_.coro_;
  if (codegen_.functionFrame().blockIsOpen()) {
    codegen_.builder_.CreateUnreachable();
  }
  llvm::Value *none = llvm::ConstantTokenNone::get(codegen_.ctx_);
  codegen_.functionFrame().switchTo(c.finalBlock);
  llvm::Value *state = codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_suspend),
                                           {none, codegen_.builder_.getTrue()}, "final");
  llvm::BasicBlock *resumedBlock = codegen_.functionFrame().newBlock("coro.resumed.done");
  llvm::SwitchInst *next = codegen_.builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(codegen_.builder_.getInt8(0), resumedBlock);
  next->addCase(codegen_.builder_.getInt8(1), c.freeBlock);
  codegen_.functionFrame().switchTo(resumedBlock);
  codegen_.builder_.CreateUnreachable();
  codegen_.functionFrame().switchTo(c.freeBlock);
  codegen_.builder_.CreateBr(c.returnBlock);
  codegen_.functionFrame().switchTo(c.returnBlock);
  codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_end),
                      {c.handle, codegen_.builder_.getFalse(), none});
  // Only the ramp goes on after coro.end: it gives the promise to the
  // caller, or drops it for a callback whose promise nobody takes.
  if (codegen_.async_->dropsPromise) {
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {c.object});
    codegen_.builder_.CreateRetVoid();
  } else {
    codegen_.builder_.CreateRet(c.object);
  }
}

void AsyncLowerer::emitAsyncFunction(FunctionDecl *fn, llvm::Function *function) {
  codegen_.functionBuilder().beginFunction(function, fn);
  CodeGen::Coroutine coroutine{};
  codegen_.coro_ = &coroutine;
  CodeGen::AsyncState state{};
  codegen_.async_ = &state;
  const Type *ret = fn->sym->ret;
  state.value = ret->isPromise() ? ret->promised() : Type::voidTy();
  state.dropsPromise = !ret->isPromise();
  beginAsync();
  codegen_.functionFrame().pushScope(); // the parameters, and the environment of a closure
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    codegen_.envParam_ = codegen_.function_->getArg(0);
    if (fn->isClosure()) {
      codegen_.envLowerer().bindCaptures(fn);
    } else {
      codegen_.envParam_->setName("this");
    }
    // The function value and the object of a method may go before the
    // coroutine is done: it keeps them.
    llvm::Value *slot = codegen_.functionFrame().createObjectSlot("env");
    codegen_.builder_.CreateCall(codegen_.objRetainFn_, {codegen_.envParam_});
    codegen_.builder_.CreateStore(codegen_.envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < codegen_.function_->arg_size(); ++i) {
    args.push_back(codegen_.function_->getArg(i));
  }
  codegen_.functionBuilder().emitParams(fn, args);
  // Its pieces may run on other threads while the caller goes on with what
  // it passed: shared (runtime/core/publish/publish.c).
  if (codegen_.parallel_ && (codegen_.aheadFunctions_.contains(fn) || fn->awaitsAhead)) {
    if (fn->isClosure() || fn->takesThis()) {
      codegen_.builder_.CreateCall(
          codegen_.processLowerer().processFunction("tz_publish_object", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}),
          {codegen_.envParam_});
    }
    for (size_t i = 0; i < fn->params.size(); ++i) {
      const Param &param = fn->params[i];
      codegen_.storeLowerer().publishValue(args[i], param.defaultValue ? Type::optional(param.sym->type) : param.sym->type);
    }
  }
  if (codegen_.recursive_.contains(fn)) {
    codegen_.throwLowerer().emitStackCheck(fn->loc());
  }
  codegen_.stmtLowerer().emitStmt(fn->body);
  if (codegen_.functionFrame().blockIsOpen()) {
    emitAsyncReturn(nullptr);
  }
  codegen_.unwindLowerer().finishScopeUnwind(0);
  codegen_.scopes_.pop_back(); // every way out released it
  codegen_.unwindLowerer().finishFunctionUnwind();
  finishAsync();
  codegen_.coro_ = nullptr;
  codegen_.async_ = nullptr;
  codegen_.function_ = nullptr;
}

// The top-level code of a program that awaits: a coroutine of its own,
// whose promise is that of the code.
llvm::Function *AsyncLowerer::emitAsyncMain(SourceFile &file) {
  auto *body =
      llvm::Function::Create(llvm::FunctionType::get(codegen_.builder_.getPtrTy(), false),
                             llvm::GlobalValue::InternalLinkage, "tz.main.async", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(body);
  codegen_.functionBuilder().beginFunction(body, nullptr);
  CodeGen::Coroutine coroutine{};
  codegen_.coro_ = &coroutine;
  CodeGen::AsyncState state{};
  codegen_.async_ = &state;
  state.value = Type::voidTy();
  beginAsync();
  codegen_.functionFrame().pushScope();
  for (Stmt *item : file.items) {
    if (!isa<FunctionDecl>(item)) {
      codegen_.stmtLowerer().emitStmt(item);
    }
  }
  if (codegen_.functionFrame().blockIsOpen()) {
    emitAsyncReturn(nullptr);
  }
  codegen_.unwindLowerer().finishScopeUnwind(0);
  codegen_.scopes_.pop_back();
  codegen_.unwindLowerer().finishFunctionUnwind();
  finishAsync();
  codegen_.coro_ = nullptr;
  codegen_.async_ = nullptr;
  codegen_.function_ = nullptr;

  return body;
}

void AsyncLowerer::emitAsyncReturn(llvm::Value *value) {
  CodeGen::AsyncState &a = *codegen_.async_;
  llvm::Value *object = a.object;
  if (value && !codegen_.promiseLowerer().promiseValueSize(a.value)) {
    value = nullptr;
  }
  if (value) {
    codegen_.builder_.CreateStore(value, codegen_.promiseLowerer().promiseValueAddress(object));
  }
  codegen_.functionFrame().emitCleanups(0);
  codegen_.promiseLowerer().fulfillPromise(object, nullptr, a.value);
  codegen_.builder_.CreateBr(codegen_.coro_->finalBlock);
  codegen_.functionFrame().startDeadBlock();
}

void AsyncLowerer::emitAsyncUnwind() {
  llvm::Value *error = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.throwLowerer().thrownSlot(), "error");
  codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), codegen_.throwLowerer().thrownSlot());
  if (codegen_.async_->generator) { // the step it ran for is rejected
    codegen_.asyncGeneratorLowerer().finishAsyncGenerator(error);
  } else {
    codegen_.promiseLowerer().rejectPromise(codegen_.async_->object, error);
  }
  codegen_.builder_.CreateBr(codegen_.coro_->finalBlock);
}

void AsyncLowerer::emitAsyncReturnStmt(ReturnStmt *stmt) {
  Expr *value = stmt->value;
  if (!value) {
    codegen_.tryLowerer().emitReturnOf(nullptr);
    return;
  }
  if (value->type->isPromise()) {
    // `return p`: the function's promise follows p, a turn of the queue
    // later, as in JavaScript. Through a `finally`, which runs first, it
    // waits for p here instead.
    if (!codegen_.tryLowerer().finallyBetween(~size_t(0))) {
      llvm::Value *other = codegen_.exprLowerer().emitExpr(value);
      codegen_.functionFrame().emitCleanups(0);
      codegen_.promiseLowerer().followPromise(codegen_.async_->object, other);
      codegen_.builder_.CreateBr(codegen_.coro_->finalBlock);
      codegen_.functionFrame().startDeadBlock();
      return;
    }
    llvm::Value *result = awaitPromise(codegen_.exprLowerer().emitExpr(value), value->type, stmt->loc());
    codegen_.tryLowerer().emitReturnOf(codegen_.promiseLowerer().promiseValueSize(codegen_.async_->value) ? result : nullptr);
    return;
  }
  llvm::Value *result = codegen_.exprLowerer().emitExpr(value);
  if (!codegen_.promiseLowerer().promiseValueSize(codegen_.async_->value) && !value->type->isVoid()) {
    codegen_.refCounts().releaseValue(result, value->type); // a callback whose promise is dropped
    result = nullptr;
  }
  codegen_.tryLowerer().emitReturnOf(value->type->isVoid() ? nullptr : result);
}

// The places of an error that comes out of an `await`: "async f", as V8
// names them.
llvm::Constant *AsyncLowerer::asyncFrameDescriptor(llvm::SMLoc loc) {
  std::string name = codegen_.unwindLowerer().frameFunctionName();
  name = name.empty() ? "async" : "async " + name;

  return codegen_.unwindLowerer().frameDescriptor(loc, &name);
}

// Waits for the coroutine's turn: the runtime queues its reactor once
// `promise` (null: none) settles. The piece after an await may run ahead of
// its turn (aheadStarts_): the runtime marks the reactor, once this piece
// has its turn.
void AsyncLowerer::suspendAsync(llvm::Value *promise, llvm::SMLoc loc) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  uint32_t site = awaiting_ ? codegen_.aheadStarts_.lookup(awaiting_) : 0;
  codegen_.builder_.CreateCall(
      asyncFunction("tz_async_await", codegen_.builder_.getVoidTy(), {ptr, ptr, codegen_.builder_.getInt32Ty()}),
      {codegen_.async_->reactor, promise ? promise : llvm::ConstantPointerNull::get(ptr),
       codegen_.builder_.getInt32(site)});
  // Where top-level code waits, for the warning about an await that never
  // ends: after the call, which waits for the turn.
  if (!codegen_.currentFn_) {
    auto *where = cast<llvm::GlobalVariable>(codegen_.module_->getOrInsertGlobal("tz_main_await", ptr));
    codegen_.builder_.CreateStore(codegen_.unwindLowerer().frameDescriptor(loc), where);
  }
  codegen_.coroutineLowerer().emitSuspend();
}

llvm::Value *AsyncLowerer::awaitPromise(llvm::Value *promise, const Type *type, llvm::SMLoc loc) {
  const Type *value = type->promised();
  suspendAsync(promise, loc);
  llvm::Value *state =
      codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), codegen_.promiseLowerer().promiseField(promise, kState), "state");
  llvm::BasicBlock *fulfilledBlock = codegen_.functionFrame().newBlock("await.fulfilled");
  llvm::BasicBlock *rejectedBlock = codegen_.functionFrame().newBlock("await.rejected");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(state, codegen_.builder_.getInt32(kFulfilled)), fulfilledBlock,
                        rejectedBlock, llvm::MDBuilder(codegen_.ctx_).createLikelyBranchWeights());
  // Rejected: its error goes up from here, with this place.
  codegen_.functionFrame().switchTo(rejectedBlock);
  llvm::Value *error =
      codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.promiseLowerer().promiseField(promise, kError), "error");
  codegen_.groupLowerer().retainObject(error);
  codegen_.refCounts().releaseValue(promise, type);
  codegen_.builder_.CreateStore(error, codegen_.throwLowerer().thrownSlot());
  codegen_.builder_.CreateCall(
      codegen_.processLowerer().processFunction("tz_error_frame", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}),
      {asyncFrameDescriptor(loc)});
  codegen_.builder_.CreateBr(codegen_.unwindLowerer().unwindTarget());
  codegen_.functionFrame().switchTo(fulfilledBlock);
  llvm::Value *result = nullptr;
  if (codegen_.promiseLowerer().promiseValueSize(value)) {
    result = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(value), codegen_.promiseLowerer().promiseValueAddress(promise), "awaited");
    codegen_.refCounts().retainValue(result, value);
  }
  codegen_.refCounts().releaseValue(promise, type);

  return result;
}

llvm::Value *AsyncLowerer::emitAwait(AwaitExpr *await) {
  Expr *operand = await->value;
  // `await __tz_ahead(p)` in the built-in modules: the piece after it may
  // run ahead of its turn (runtime/core/parallel/parallel.c), as the code
  // guarantees.
  if (auto *call = dyn_cast<CallExpr>(operand); call && call->builtin == Builtin::LibAhead) {
    if (codegen_.parallel_) {
      codegen_.aheadStarts_.try_emplace(await, uint32_t(codegen_.aheadStarts_.size() + 1));
    }
    operand = call->args[0];
  }
  // `await Promise.resolve(x)` waits as `await x` does, one turn of the
  // queue, without the promise.
  if (const Expr *inner = resolvedValue(operand)) {
    operand = const_cast<Expr *>(inner);
  }
  const Type *type = operand->type;
  llvm::Value *value = codegen_.exprLowerer().emitExpr(operand);
  const AwaitExpr *outer = std::exchange(awaiting_, await);
  llvm::Value *result;
  if (type->isPromise()) {
    result = awaitPromise(value, type, await->loc());
  } else if (holdsPromise(type)) {
    result = awaitUnion(value, type, await->type, await->loc());
  } else {
    suspendAsync(nullptr, await->loc());
    result = type->isVoid() ? nullptr : value;
  }
  awaiting_ = outer;

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

// `await` of a union (or `T | undefined`) that may hold a Promise, as in
// JavaScript: it waits for the Promise, or a turn of the queue for any
// other value.
llvm::Value *AsyncLowerer::awaitUnion(llvm::Value *value, const Type *type, const Type *result,
                                 llvm::SMLoc loc) {
  llvm::Type *resultType = result->isVoid() ? nullptr : codegen_.moduleBuilder().lower(result);
  auto plain = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    suspendAsync(nullptr, loc);
    if (!resultType) {
      if (v) {
        codegen_.refCounts().releaseValue(v, t);
      }
      return nullptr;
    }

    return v ? codegen_.exprLowerer().emitConversion(v, t, result) : codegen_.optionalLowerer().undefinedValue(result);
  };
  auto promise = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    llvm::Value *got = awaitPromise(v, t, loc);
    if (!resultType) {
      if (got) {
        codegen_.refCounts().releaseValue(got, t->promised());
      }
      return nullptr;
    }

    return codegen_.exprLowerer().emitConversion(got, t->promised(), result);
  };
  auto part = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    if (t->isPromise()) {
      return promise(v, t);
    }
    if (holdsPromise(t)) {
      return awaitUnion(v, t, result, loc);
    }

    return plain(v, t);
  };
  if (type->isTagged()) {
    return codegen_.unionLayout().forEachSlot(
        value, type->slots(),
        [&](UnionSlot slot) { return part(codegen_.unionLayout().taggedPart(value, type, slot), type->slotPart(slot)); },
        resultType, resultType ? llvm::Constant::getNullValue(resultType) : nullptr);
  }
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("await.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  auto finish = [&](llvm::Value *v) {
    incoming.push_back({v, codegen_.builder_.GetInsertBlock()});
    codegen_.builder_.CreateBr(endBlock);
  };
  llvm::BasicBlock *promiseBlock = codegen_.functionFrame().newBlock("await.promise");
  llvm::BasicBlock *otherBlock = codegen_.functionFrame().newBlock("await.other");
  if (type->isOptional()) {
    codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(value, type), promiseBlock, otherBlock);
    codegen_.functionFrame().switchTo(otherBlock);
    finish(plain(nullptr, type));
    codegen_.functionFrame().switchTo(promiseBlock);
    finish(part(codegen_.optionalLowerer().optionalValue(value, type), type->inner()));
  } else {
    // Objects, one of them a Promise: told apart by the number of its layout.
    const Type *promiseType =
        *llvm::find_if(type->members(), [](const Type *m) { return m->isPromise(); });
    codegen_.builder_.CreateCondBr(codegen_.unionLowerer().emitIsOneOf(codegen_.unionLowerer().objectClassId(value), promiseType), promiseBlock, otherBlock);
    codegen_.functionFrame().switchTo(promiseBlock);
    finish(promise(value, promiseType));
    codegen_.functionFrame().switchTo(otherBlock);
    finish(plain(value, unionWithout(type, promiseType)));
  }
  codegen_.functionFrame().switchTo(endBlock);
  if (!resultType) {
    return nullptr;
  }
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(resultType, unsigned(incoming.size()));
  for (auto [v, block] : incoming) {
    phi->addIncoming(v, block);
  }

  return phi;
}

} // namespace tz
