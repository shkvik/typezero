#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
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
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

constexpr uint64_t kPromiseHeader = 40;

constexpr uint64_t kReactorSize = 40;

constexpr uint32_t kAsyncFrameAlign = 8;

const Expr *resolvedValue(const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  if (!call || call->builtin != Builtin::PromiseResolve || call->args.size() != 1 ||
      call->args[0]->type->isPromise()) {
    return nullptr;
  }

  return call->args[0];
}

}

AsyncLowerer::AsyncLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee AsyncLowerer::asyncFunction(llvm::StringRef name, llvm::Type *ret,
                                                 llvm::ArrayRef<llvm::Type *> params) {
  return lowering_.processLowerer().processFunction(name, ret, params);
}

llvm::Function *AsyncLowerer::asyncResumeFunction() {
  if (asyncResumeFn_) {
    return asyncResumeFn_;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  asyncResumeFn_ = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.async.resume", lowering_.module_.get());
  asyncResumeFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", asyncResumeFn_));
  llvm::Value *frame =
      b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), asyncResumeFn_->getArg(0), kReactorSize, "frame");
  b.CreateCall(lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  b.CreateRetVoid();

  return asyncResumeFn_;
}

void AsyncLowerer::beginAsync() {
  lowering_.function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  Lowering::Coroutine &c = *lowering_.coro_;
  Lowering::AsyncState &a = *lowering_.async_;
  llvm::Type *i8 = lowering_.builder_.getInt8Ty(), *i64 = lowering_.builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  uint64_t reactorOffset = kPromiseHeader + lowering_.promiseLowerer().promiseValueSize(a.value);
  uint64_t frameOffset = reactorOffset + kReactorSize;
  c.id = lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_id),
      {lowering_.builder_.getInt32(kAsyncFrameAlign), null, null, null}, "id");
  llvm::Value *size = lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {}, "size");
  bool member = lowering_.memory().promiseIsMember(a.value);
  llvm::Value *total = lowering_.builder_.CreateAdd(size, lowering_.builder_.getInt64(frameOffset));
  llvm::Value *memory =
      member ? lowering_.builder_.CreateCall(
                   lowering_.groupLowerer().groupFunction("tz_group_alloc",
                                                          lowering_.builder_.getPtrTy(), {i64}),
                   {total}, "memory")
             : lowering_.builder_.CreateCall(lowering_.rt_.objAlloc, {total}, "memory");
  lowering_.builder_.CreateMemSet(lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8),
                                  lowering_.builder_.getInt8(0), frameOffset - 8,
                                  llvm::MaybeAlign(8));
  if (!member) {
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1), memory);
  }
  lowering_.builder_.CreateStore(lowering_.promiseLowerer().promiseLayout(a.value),
                                 lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor = lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, reactorOffset);
  lowering_.builder_.CreateStore(asyncResumeFunction(),
                                 lowering_.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  lowering_.builder_.CreateStore(memory,
                                 lowering_.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame =
      lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, frameOffset, "frame");
  c.handle = lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_begin), {c.id, frame},
      "handle");
  c.object = lowering_.builder_.CreateGEP(
      i8, c.handle, llvm::ConstantInt::getSigned(i64, -int64_t(frameOffset)), "promise");
  a.object = c.object;
  a.reactor = lowering_.builder_.CreateConstInBoundsGEP1_64(i8, c.object, reactorOffset, "reactor");
  c.finalBlock = lowering_.functionFrame().newBlock("coro.final");
  c.freeBlock = lowering_.functionFrame().newBlock("coro.free");
  c.returnBlock = lowering_.functionFrame().newBlock("coro.return");
}

void AsyncLowerer::finishAsync() {
  Lowering::Coroutine &c = *lowering_.coro_;
  if (lowering_.functionFrame().blockIsOpen()) {
    lowering_.builder_.CreateUnreachable();
  }
  llvm::Value *none = llvm::ConstantTokenNone::get(lowering_.ctx_);
  lowering_.functionFrame().switchTo(c.finalBlock);
  llvm::Value *state = lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_suspend),
      {none, lowering_.builder_.getTrue()}, "final");
  llvm::BasicBlock *resumedBlock = lowering_.functionFrame().newBlock("coro.resumed.done");
  llvm::SwitchInst *next = lowering_.builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(lowering_.builder_.getInt8(0), resumedBlock);
  next->addCase(lowering_.builder_.getInt8(1), c.freeBlock);
  lowering_.functionFrame().switchTo(resumedBlock);
  lowering_.builder_.CreateUnreachable();
  lowering_.functionFrame().switchTo(c.freeBlock);
  lowering_.builder_.CreateBr(c.returnBlock);
  lowering_.functionFrame().switchTo(c.returnBlock);
  lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_end),
      {c.handle, lowering_.builder_.getFalse(), none});
  if (lowering_.async_->dropsPromise) {
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {c.object});
    lowering_.builder_.CreateRetVoid();
  } else {
    lowering_.builder_.CreateRet(c.object);
  }
}

void AsyncLowerer::emitAsyncFunction(FunctionDecl *fn, llvm::Function *function) {
  lowering_.functionBuilder().beginFunction(function, fn);
  Lowering::Coroutine coroutine{};
  lowering_.coro_ = &coroutine;
  Lowering::AsyncState state{};
  lowering_.async_ = &state;
  const Type *ret = fn->sym->ret;
  state.value = ret->isPromise() ? ret->promised() : Type::voidTy();
  state.dropsPromise = !ret->isPromise();
  beginAsync();
  lowering_.functionFrame().pushScope();
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    lowering_.envParam_ = lowering_.function_->getArg(0);
    if (fn->isClosure()) {
      lowering_.envLowerer().bindCaptures(fn);
    } else {
      lowering_.envParam_->setName("this");
    }
    llvm::Value *slot = lowering_.functionFrame().createObjectSlot("env");
    lowering_.builder_.CreateCall(lowering_.objRetainFn_, {lowering_.envParam_});
    lowering_.builder_.CreateStore(lowering_.envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < lowering_.function_->arg_size(); ++i) {
    args.push_back(lowering_.function_->getArg(i));
  }
  lowering_.functionBuilder().emitParams(fn, args);
  if (lowering_.parallel_ && (lowering_.aheadFunctions_.contains(fn) || fn->awaitsAhead)) {
    if (fn->isClosure() || fn->takesThis()) {
      lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction(
              "tz_publish_object", lowering_.builder_.getVoidTy(), {lowering_.builder_.getPtrTy()}),
          {lowering_.envParam_});
    }
    for (size_t i = 0; i < fn->params.size(); ++i) {
      const Param &param = fn->params[i];
      lowering_.storeLowerer().publishValue(
          args[i], param.defaultValue ? Type::optional(param.sym->type) : param.sym->type);
    }
  }
  lowering_.stmtLowerer().emitStmt(fn->body);
  if (lowering_.functionFrame().blockIsOpen()) {
    emitAsyncReturn(nullptr);
  }
  lowering_.scopes_.pop_back();
  finishAsync();
  lowering_.coro_ = nullptr;
  lowering_.async_ = nullptr;
  lowering_.function_ = nullptr;
}

llvm::Function *AsyncLowerer::emitAsyncMain(SourceFile &file) {
  auto *body = llvm::Function::Create(llvm::FunctionType::get(lowering_.builder_.getPtrTy(), false),
                                      llvm::GlobalValue::InternalLinkage, "tz.main.async",
                                      lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(body);
  lowering_.functionBuilder().beginFunction(body, nullptr);
  Lowering::Coroutine coroutine{};
  lowering_.coro_ = &coroutine;
  Lowering::AsyncState state{};
  lowering_.async_ = &state;
  state.value = Type::voidTy();
  beginAsync();
  lowering_.functionFrame().pushScope();
  for (Stmt *item : file.items) {
    if (!isa<FunctionDecl>(item)) {
      lowering_.stmtLowerer().emitStmt(item);
    }
  }
  if (lowering_.functionFrame().blockIsOpen()) {
    emitAsyncReturn(nullptr);
  }
  lowering_.scopes_.pop_back();
  finishAsync();
  lowering_.coro_ = nullptr;
  lowering_.async_ = nullptr;
  lowering_.function_ = nullptr;

  return body;
}

void AsyncLowerer::emitAsyncReturn(llvm::Value *value) {
  Lowering::AsyncState &a = *lowering_.async_;
  llvm::Value *object = a.object;
  if (value && !lowering_.promiseLowerer().promiseValueSize(a.value)) {
    value = nullptr;
  }
  if (value) {
    lowering_.builder_.CreateStore(value, lowering_.promiseLowerer().promiseValueAddress(object));
  }
  lowering_.functionFrame().emitCleanups(0);
  lowering_.promiseLowerer().fulfillPromise(object, nullptr, a.value);
  lowering_.builder_.CreateBr(lowering_.coro_->finalBlock);
  lowering_.functionFrame().startDeadBlock();
}

void AsyncLowerer::emitAsyncReturnStmt(ReturnStmt *stmt) {
  Expr *value = stmt->value;
  if (!value) {
    lowering_.exitLowerer().emitReturnOf(nullptr);
    return;
  }
  if (value->type->isPromise()) {
    llvm::Value *other = lowering_.exprLowerer().emitExpr(value);
    lowering_.functionFrame().emitCleanups(0);
    lowering_.promiseLowerer().followPromise(lowering_.async_->object, other);
    lowering_.builder_.CreateBr(lowering_.coro_->finalBlock);
    lowering_.functionFrame().startDeadBlock();
    return;
  }
  llvm::Value *result = lowering_.exprLowerer().emitExpr(value);
  if (!lowering_.promiseLowerer().promiseValueSize(lowering_.async_->value) &&
      !value->type->isVoid()) {
    lowering_.refCounts().releaseValue(result, value->type);
    result = nullptr;
  }
  lowering_.exitLowerer().emitReturnOf(value->type->isVoid() ? nullptr : result);
}

void AsyncLowerer::suspendAsync(llvm::Value *promise, llvm::SMLoc loc) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  uint32_t site = awaiting_ ? lowering_.aheadStarts_.lookup(awaiting_) : 0;
  lowering_.builder_.CreateCall(asyncFunction("tz_async_await", lowering_.builder_.getVoidTy(),
                                              {ptr, ptr, lowering_.builder_.getInt32Ty()}),
                                {lowering_.async_->reactor,
                                 promise ? promise : llvm::ConstantPointerNull::get(ptr),
                                 lowering_.builder_.getInt32(site)});
  if (!lowering_.currentFn_) {
    auto *where =
        cast<llvm::GlobalVariable>(lowering_.module_->getOrInsertGlobal("tz_main_await", ptr));
    lowering_.builder_.CreateStore(lowering_.placeLowerer().frameDescriptor(loc), where);
  }
  lowering_.coroutineLowerer().emitSuspend();
}

llvm::Value *AsyncLowerer::awaitPromise(llvm::Value *promise, const Type *type, llvm::SMLoc loc) {
  const Type *value = type->promised();
  suspendAsync(promise, loc);
  llvm::Value *result = nullptr;
  if (lowering_.promiseLowerer().promiseValueSize(value)) {
    result = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(value),
                                           lowering_.promiseLowerer().promiseValueAddress(promise),
                                           "awaited");
    lowering_.refCounts().retainValue(result, value);
  }
  lowering_.refCounts().releaseValue(promise, type);

  return result;
}

llvm::Value *AsyncLowerer::emitAwait(AwaitExpr *await) {
  Expr *operand = await->value;
  if (auto *call = dyn_cast<CallExpr>(operand); call && call->builtin == Builtin::LibAhead) {
    if (lowering_.parallel_) {
      lowering_.aheadStarts_.try_emplace(await, uint32_t(lowering_.aheadStarts_.size() + 1));
    }
    operand = call->args[0];
  }
  if (const Expr *inner = resolvedValue(operand)) {
    operand = const_cast<Expr *>(inner);
  }
  const Type *type = operand->type;
  llvm::Value *value = lowering_.exprLowerer().emitExpr(operand);
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

llvm::Value *AsyncLowerer::awaitUnion(llvm::Value *value, const Type *type, const Type *result,
                                      llvm::SMLoc loc) {
  llvm::Type *resultType = result->isVoid() ? nullptr : lowering_.moduleBuilder().lower(result);
  auto plain = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    suspendAsync(nullptr, loc);
    if (!resultType) {
      if (v) {
        lowering_.refCounts().releaseValue(v, t);
      }
      return nullptr;
    }

    return v ? lowering_.exprLowerer().emitConversion(v, t, result)
             : lowering_.optionalLowerer().undefinedValue(result);
  };
  auto promise = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    llvm::Value *got = awaitPromise(v, t, loc);
    if (!resultType) {
      if (got) {
        lowering_.refCounts().releaseValue(got, t->promised());
      }
      return nullptr;
    }

    return lowering_.exprLowerer().emitConversion(got, t->promised(), result);
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
    return lowering_.unionLayout().forEachSlot(
        value, type->slots(),
        [&](UnionSlot slot) {
          return part(lowering_.unionLayout().taggedPart(value, type, slot), type->slotPart(slot));
        },
        resultType, resultType ? llvm::Constant::getNullValue(resultType) : nullptr);
  }
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("await.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  auto finish = [&](llvm::Value *v) {
    incoming.push_back({v, lowering_.builder_.GetInsertBlock()});
    lowering_.builder_.CreateBr(endBlock);
  };
  llvm::BasicBlock *promiseBlock = lowering_.functionFrame().newBlock("await.promise");
  llvm::BasicBlock *otherBlock = lowering_.functionFrame().newBlock("await.other");
  if (type->isOptional()) {
    lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(value, type),
                                    promiseBlock, otherBlock);
    lowering_.functionFrame().switchTo(otherBlock);
    finish(plain(nullptr, type));
    lowering_.functionFrame().switchTo(promiseBlock);
    finish(part(lowering_.optionalLowerer().optionalValue(value, type), type->inner()));
  } else {
    const Type *promiseType =
        *llvm::find_if(type->members(), [](const Type *m) { return m->isPromise(); });
    lowering_.builder_.CreateCondBr(lowering_.unionLowerer().emitIsOneOf(
                                        lowering_.unionLowerer().objectClassId(value), promiseType),
                                    promiseBlock, otherBlock);
    lowering_.functionFrame().switchTo(promiseBlock);
    finish(promise(value, promiseType));
    lowering_.functionFrame().switchTo(otherBlock);
    finish(plain(value, unionWithout(type, promiseType)));
  }
  lowering_.functionFrame().switchTo(endBlock);
  if (!resultType) {
    return nullptr;
  }
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(resultType, unsigned(incoming.size()));
  for (auto [v, block] : incoming) {
    phi->addIncoming(v, block);
  }

  return phi;
}

}
