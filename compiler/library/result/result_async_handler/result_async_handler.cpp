#include "compiler/library/result/result_async_handler/result_async_handler.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
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

enum PromiseField : unsigned { kRc, kLayout, kHead, kTail, kError, kState, kFlags };

constexpr uint32_t kFulfilled = 1;

// A reaction to a promise (then, catch, finally): { i64 rc, ptr descriptor,
// reactor { ptr next, ptr run, ptr owner, ptr source, i64 ahead }, ptr
// derived, fn onFulfilled, fn onRejected }.
enum ReactionField : unsigned {
  kReactionRc,
  kReactionDesc,
  kReactorNext,
  kReactorRun,
  kReactorOwner,
  kReactorSource,
  kReactorAhead,
  kReactionDerived,
  kReactionFulfilled,
  kReactionRejected,
};

constexpr uint64_t kReactorOffset = 16;

} // namespace

ResultAsyncHandler::ResultAsyncHandler(CodeGen &codegen) : codegen_(codegen) {}

// The code of a reaction of a ResultAsync, once the promise it waits for
// (`source`, of a value of `value`) settled: it settles its own promise, of
// a value of `result`, as the method `kind` does with the callbacks of types
// `first` and `second`. It takes the turns of the queue that neverthrow's
// code takes, so that what else runs meanwhile runs in the same order: map,
// mapErr, orElse and the tees run an async function, whose promise the
// ResultAsync then follows; andThen, match and unwrapOr are a callback of
// then(); fromPromise is a then() and a catch().
llvm::Function *ResultAsyncHandler::resultAsyncHandler(Builtin kind, const Type *value, const Type *result,
                                            const Type *first, const Type *second) {
  llvm::Function *&handler = codegen_.reactionHandlers_[{kind, value, result, first, second}];
  if (handler) {
    return handler;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  handler =
      llvm::Function::Create(llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr, ptr}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.result.async", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(handler);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = std::exchange(codegen_.function_, handler);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", handler));
  llvm::StructType *type = codegen_.reactionLowerer().reactionType();
  llvm::Value *object = codegen_.builder_.CreateInBoundsGEP(
      codegen_.builder_.getInt8Ty(), handler->getArg(0),
      llvm::ConstantInt::getSigned(codegen_.builder_.getInt64Ty(), -int64_t(kReactorOffset)), "reaction");
  llvm::Value *source = handler->getArg(1);
  llvm::Value *derived =
      codegen_.builder_.CreateLoad(ptr, codegen_.builder_.CreateStructGEP(type, object, kReactionDerived), "derived");
  const Type *errorType = codegen_.errorClass_->type;
  bool holds = codegen_.promiseLowerer().promiseValueSize(result) != 0;
  llvm::FunctionCallee fulfillLater =
      codegen_.asyncLowerer().asyncFunction("tz_promise_fulfill_later", codegen_.builder_.getVoidTy(), {ptr});
  // The promise the code settles: that of the async function, which this
  // one follows once the code is done, or this one.
  llvm::Value *settled = derived;
  auto done = [&] {
    if (settled != derived) {
      codegen_.promiseLowerer().followPromise(derived, settled);
    }
    codegen_.builder_.CreateRetVoid();
  };

  auto callback = [&](unsigned field) {
    return codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(type, object, field), "callback");
  };
  // Calls `fn` of `fnType` with the owned `arg` of `argType`, as a half of a
  // Result is given (undefined for void).
  auto callWith = [&](llvm::Value *fn, const Type *fnType, llvm::Value *arg, const Type *argType) {
    std::vector<llvm::Value *> args;
    if (fnType->params().empty()) {
      if (arg) {
        codegen_.refCounts().releaseValue(arg, argType);
      }
    } else {
      const Type *param = fnType->params()[0];
      const Type *given = argType->isVoid() ? Type::undefinedTy() : argType;
      llvm::Value *v = argType->isVoid() || !arg ? codegen_.builder_.getFalse() : arg;
      args.push_back(given == param ? v : codegen_.exprLowerer().emitConversion(v, given, param));
    }

    return codegen_.closureLowerer().callFunctionValue(fn, fnType, args);
  };
  // After a callback of `fnType` that may throw: `onThrow` takes its error,
  // and the code is done.
  auto threw = [&](const Type *fnType, llvm::function_ref<void(llvm::Value *)> onThrow) {
    if (!codegen_.typeThrows(fnType)) {
      return;
    }
    llvm::Value *error = codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot(), "thrown");
    llvm::BasicBlock *threwBlock = codegen_.functionFrame().newBlock("callback.threw");
    llvm::BasicBlock *gaveBlock = codegen_.functionFrame().newBlock("callback.gave");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(error), threwBlock, gaveBlock,
                          llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
    codegen_.functionFrame().switchTo(threwBlock);
    codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), codegen_.throwLowerer().thrownSlot());
    onThrow(error);
    done();
    codegen_.functionFrame().switchTo(gaveBlock);
  };
  auto reject = [&](llvm::Value *error) { codegen_.promiseLowerer().rejectPromise(settled, error); };
  // A Result of `result`: an Ok (or an Err) of the owned half `v` of type
  // `from`.
  auto made = [&](bool ok, llvm::Value *v, const Type *from) {
    const Type *to = ok ? result->okType() : result->errType();
    llvm::Value *half = v && !from->isVoid() ? codegen_.resultLowerer().convertHalf(v, from, to) : nullptr;

    return codegen_.resultLowerer().makeResult(result, codegen_.builder_.getInt1(ok), ok ? half : nullptr, ok ? nullptr : half);
  };
  // What a callback of andThen, orElse or match gave, of type `given`: a
  // promise to follow, or the value.
  auto settleWith = [&](llvm::Value *v, const Type *given) {
    if (given->isPromise() && given->promised() == result) {
      codegen_.promiseLowerer().followPromise(settled, v);
    } else if (given->isPromise()) {
      // Its value converts on the way, which takes the turn of the queue
      // that following it would.
      codegen_.reactionLowerer().attachReaction(
          v, settled,
          codegen_.reactionLowerer().reactionHandler(Builtin::PromiseThen, given->promised(), result, nullptr, nullptr),
          nullptr, nullptr, /*later=*/true);
      codegen_.refCounts().releaseValue(v, given);
    } else {
      codegen_.promiseLowerer().fulfillPromise(
          settled, holds && !given->isVoid() ? codegen_.exprLowerer().emitConversion(v, given, result) : nullptr, result);
    }
    done();
  };

  llvm::Value *state =
      codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), codegen_.promiseLowerer().promiseField(source, kState), "state");
  llvm::BasicBlock *fulfilledBlock = codegen_.functionFrame().newBlock("fulfilled");
  llvm::BasicBlock *rejectedBlock = codegen_.functionFrame().newBlock("rejected");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(state, codegen_.builder_.getInt32(kFulfilled)), fulfilledBlock,
                        rejectedBlock);

  // Rejected: the error goes on, unless the code catches it.
  codegen_.functionFrame().switchTo(rejectedBlock);
  llvm::Value *error = codegen_.builder_.CreateLoad(ptr, codegen_.promiseLowerer().promiseField(source, kError), "error");
  codegen_.groupLowerer().retainObject(error);
  switch (kind) {
  case Builtin::ResultAsyncFromPromise:
  case Builtin::ResultAsyncFromThrowable:
    // An Err of what toError makes of the error, or of the error.
    if (first) {
      llvm::Value *x = callWith(callback(kReactionFulfilled), first, error, errorType);
      threw(first, reject);
      codegen_.promiseLowerer().fulfillPromise(derived, made(false, x, first->ret()), result);
    } else {
      codegen_.promiseLowerer().fulfillPromise(derived, made(false, error, errorType), result);
    }
    break;
  case Builtin::ResultAsyncTeeDone:
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {error}); // a tee does not care
    codegen_.promiseLowerer().fulfillPromise(derived, nullptr, result);         // the Result is in the value already
    break;
  case Builtin::ResultAsyncUnwrapOr:
    if (holds) {
      // The default waits in the value: it goes.
      llvm::Value *slot = codegen_.promiseLowerer().promiseValueAddress(derived);
      codegen_.refCounts().releaseValue(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(result), slot), result);
      codegen_.builder_.CreateStore(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(result)), slot);
    }
    codegen_.promiseLowerer().rejectPromise(derived, error);
    break;
  default:
    codegen_.promiseLowerer().rejectPromise(derived, error);
    break;
  }
  codegen_.builder_.CreateRetVoid();

  // Fulfilled.
  codegen_.functionFrame().switchTo(fulfilledBlock);
  llvm::Value *given = nullptr;
  if (codegen_.promiseLowerer().promiseValueSize(value)) {
    given = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(value), codegen_.promiseLowerer().promiseValueAddress(source), "value");
    codegen_.refCounts().retainValue(given, value);
  }
  switch (kind) {
  case Builtin::ResultAsyncFromPromise: // its catch(): the Result goes on
    codegen_.promiseLowerer().fulfillPromise(derived, given, result);
    codegen_.builder_.CreateRetVoid();
    break;
  case Builtin::ResultAsyncFromSafePromise:
  case Builtin::ResultAsyncFromThrowable:
  case Builtin::ResultAsyncWrapOk:
  case Builtin::ResultAsyncWrapErr:
    codegen_.promiseLowerer().fulfillPromise(derived, made(kind != Builtin::ResultAsyncWrapErr, given, value), result);
    codegen_.builder_.CreateRetVoid();
    break;
  case Builtin::ResultAsyncTeeDone:
    if (given) {
      codegen_.refCounts().releaseValue(given, value);
    }
    codegen_.promiseLowerer().fulfillPromise(derived, nullptr, result); // the Result is in the value already
    codegen_.builder_.CreateRetVoid();
    break;
  default: {
    // A Result came: the method goes by its half.
    if (kind == Builtin::ResultAsyncMap || kind == Builtin::ResultAsyncMapErr ||
        kind == Builtin::ResultAsyncOrElse || kind == Builtin::ResultAsyncAndTee ||
        kind == Builtin::ResultAsyncOrTee) {
      settled = codegen_.promiseLowerer().newPromise(result);
    }
    const Type *okType = value->okType(), *errType = value->errType();
    llvm::Value *res = given ? given : llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(value));
    llvm::Value *okHalf = codegen_.resultLowerer().resultHalf(res, value, false), *errHalf = codegen_.resultLowerer().resultHalf(res, value, true);
    llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("ok");
    llvm::BasicBlock *errBlock = codegen_.functionFrame().newBlock("err");
    codegen_.builder_.CreateCondBr(codegen_.resultLowerer().resultIsOk(res, value), okBlock, errBlock);
    bool onError = kind == Builtin::ResultAsyncMapErr || kind == Builtin::ResultAsyncOrElse ||
                   kind == Builtin::ResultAsyncOrTee;
    bool tee = kind == Builtin::ResultAsyncAndTee || kind == Builtin::ResultAsyncOrTee;
    for (bool isOk : {true, false}) {
      codegen_.functionFrame().switchTo(isOk ? okBlock : errBlock);
      llvm::Value *half = isOk ? okHalf : errHalf;
      const Type *halfType = isOk ? okType : errType;
      if (halfType->isNever()) { // a Result without this half
        codegen_.builder_.CreateUnreachable();
        continue;
      }
      if (kind == Builtin::ResultAsyncUnwrapOr) {
        if (isOk && holds) {
          // The value instead of the default.
          llvm::Value *slot = codegen_.promiseLowerer().promiseValueAddress(derived);
          codegen_.refCounts().releaseValue(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(result), slot), result);
          codegen_.builder_.CreateStore(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(result)), slot);
          codegen_.refCounts().retainValue(half, halfType);
          codegen_.refCounts().releaseValue(res, value);
          codegen_.promiseLowerer().fulfillPromise(derived, codegen_.exprLowerer().emitConversion(half, halfType, result), result);
        } else {
          codegen_.refCounts().releaseValue(res, value);
          codegen_.promiseLowerer().fulfillPromise(derived, nullptr, result); // the default
        }
        codegen_.builder_.CreateRetVoid();
        continue;
      }
      if (kind != Builtin::ResultAsyncMatch && isOk == onError) {
        // The half nothing is called for goes on as it is.
        if (tee) {
          codegen_.promiseLowerer().fulfillPromise(settled, res, result);
        } else {
          llvm::Value *keep = halfType->isVoid() ? nullptr : half;
          codegen_.promiseLowerer().fulfillPromise(settled, made(isOk, keep, halfType), result);
        }
        done();
        continue;
      }
      bool onSecond = kind == Builtin::ResultAsyncMatch && !isOk;
      const Type *fnType = onSecond ? second : first;
      if (tee) {
        // The Result waits in the value until the callback is done.
        codegen_.refCounts().retainValue(half, halfType);
        if (holds) {
          codegen_.builder_.CreateStore(res, codegen_.promiseLowerer().promiseValueAddress(settled));
        } else {
          codegen_.refCounts().releaseValue(res, value);
        }
      }
      llvm::Value *x = callWith(callback(onSecond ? kReactionRejected : kReactionFulfilled), fnType,
                                half, halfType);
      const Type *ret = fnType->ret();
      switch (kind) {
      case Builtin::ResultAsyncMap:
      case Builtin::ResultAsyncMapErr:
        // `return ok(await f(value))`: once f's promise settles, or a turn
        // later.
        threw(fnType, reject);
        if (ret->isPromise()) {
          codegen_.reactionLowerer().attachReaction(
              x, settled,
              resultAsyncHandler(isOk ? Builtin::ResultAsyncWrapOk : Builtin::ResultAsyncWrapErr,
                                 ret->promised(), result, nullptr, nullptr),
              nullptr, nullptr);
          codegen_.refCounts().releaseValue(x, ret);
        } else {
          if (holds) {
            codegen_.builder_.CreateStore(made(isOk, ret->isVoid() ? nullptr : x, ret),
                                 codegen_.promiseLowerer().promiseValueAddress(settled));
          }
          codegen_.builder_.CreateCall(fulfillLater, {settled});
        }
        done();
        break;
      case Builtin::ResultAsyncAndTee:
      case Builtin::ResultAsyncOrTee:
        // `try { await f(value) } catch {}`, then the Result: when f
        // throws, the error goes and the Result goes on now.
        threw(fnType, [&](llvm::Value *thrown) {
          codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {thrown});
          codegen_.promiseLowerer().fulfillPromise(settled, nullptr, result);
        });
        if (ret->isPromise()) {
          codegen_.reactionLowerer().attachReaction(x, settled,
                         resultAsyncHandler(Builtin::ResultAsyncTeeDone, ret->promised(), result,
                                            nullptr, nullptr),
                         nullptr, nullptr);
          codegen_.refCounts().releaseValue(x, ret);
        } else {
          if (x && !ret->isVoid()) {
            codegen_.refCounts().releaseValue(x, ret);
          }
          codegen_.builder_.CreateCall(fulfillLater, {settled});
        }
        done();
        break;
      default: // andThen, orElse and match
        threw(fnType, reject);
        settleWith(x, ret);
        break;
      }
    }
    break;
  }
  }
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);

  return handler;
}

} // namespace tz
