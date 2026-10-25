#include "compiler/library/result/result_async_handler/result_async_handler.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
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

ResultAsyncHandler::ResultAsyncHandler(Lowering &lowering) : lowering_(lowering) {}

// The code of a reaction of a ResultAsync, once the promise it waits for
// (`source`, of a value of `value`) settled: it settles its own promise, of
// a value of `result`, as the method `kind` does with the callbacks of types
// `first` and `second`. It takes the turns of the queue that neverthrow's
// code takes, so that what else runs meanwhile runs in the same order: map,
// mapErr, orElse and the tees run an async function, whose promise the
// ResultAsync then follows; andThen, match and unwrapOr are a callback of
// then(); fromPromise is a then() and a catch().
llvm::Function *ResultAsyncHandler::resultAsyncHandler(Builtin kind, const Type *value,
                                                       const Type *result, const Type *first,
                                                       const Type *second) {
  llvm::Function *&handler = lowering_.reactionHandlers_[{kind, value, result, first, second}];
  if (handler) {
    return handler;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  handler = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.result.async", lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(handler);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, handler);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", handler));
  llvm::StructType *type = lowering_.reactionLowerer().reactionType();
  llvm::Value *object = lowering_.builder_.CreateInBoundsGEP(
      lowering_.builder_.getInt8Ty(), handler->getArg(0),
      llvm::ConstantInt::getSigned(lowering_.builder_.getInt64Ty(), -int64_t(kReactorOffset)),
      "reaction");
  llvm::Value *source = handler->getArg(1);
  llvm::Value *derived = lowering_.builder_.CreateLoad(
      ptr, lowering_.builder_.CreateStructGEP(type, object, kReactionDerived), "derived");
  const Type *errorType = lowering_.errorClass_->type;
  bool holds = lowering_.promiseLowerer().promiseValueSize(result) != 0;
  llvm::FunctionCallee fulfillLater = lowering_.asyncLowerer().asyncFunction(
      "tz_promise_fulfill_later", lowering_.builder_.getVoidTy(), {ptr});
  // The promise the code settles: that of the async function, which this
  // one follows once the code is done, or this one.
  llvm::Value *settled = derived;
  auto done = [&] {
    if (settled != derived) {
      lowering_.promiseLowerer().followPromise(derived, settled);
    }
    lowering_.builder_.CreateRetVoid();
  };

  auto callback = [&](unsigned field) {
    return lowering_.builder_.CreateLoad(
        lowering_.fnTy_, lowering_.builder_.CreateStructGEP(type, object, field), "callback");
  };
  // Calls `fn` of `fnType` with the owned `arg` of `argType`, as a half of a
  // Result is given (undefined for void).
  auto callWith = [&](llvm::Value *fn, const Type *fnType, llvm::Value *arg, const Type *argType) {
    std::vector<llvm::Value *> args;
    if (fnType->params().empty()) {
      if (arg) {
        lowering_.refCounts().releaseValue(arg, argType);
      }
    } else {
      const Type *param = fnType->params()[0];
      const Type *given = argType->isVoid() ? Type::undefinedTy() : argType;
      llvm::Value *v = argType->isVoid() || !arg ? lowering_.builder_.getFalse() : arg;
      args.push_back(given == param ? v : lowering_.exprLowerer().emitConversion(v, given, param));
    }

    return lowering_.closureLowerer().callFunctionValue(fn, fnType, args);
  };
  // After a callback of `fnType` that may throw: `onThrow` takes its error,
  // and the code is done.
  auto threw = [&](const Type *fnType, llvm::function_ref<void(llvm::Value *)> onThrow) {
    if (!lowering_.typeThrows(fnType)) {
      return;
    }
    llvm::Value *error =
        lowering_.builder_.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot(), "thrown");
    llvm::BasicBlock *threwBlock = lowering_.functionFrame().newBlock("callback.threw");
    llvm::BasicBlock *gaveBlock = lowering_.functionFrame().newBlock("callback.gave");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(error), threwBlock,
                                    gaveBlock,
                                    llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
    lowering_.functionFrame().switchTo(threwBlock);
    lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr),
                                   lowering_.throwLowerer().thrownSlot());
    onThrow(error);
    done();
    lowering_.functionFrame().switchTo(gaveBlock);
  };
  auto reject = [&](llvm::Value *error) {
    lowering_.promiseLowerer().rejectPromise(settled, error);
  };
  // A Result of `result`: an Ok (or an Err) of the owned half `v` of type
  // `from`.
  auto made = [&](bool ok, llvm::Value *v, const Type *from) {
    const Type *to = ok ? result->okType() : result->errType();
    llvm::Value *half =
        v && !from->isVoid() ? lowering_.resultLowerer().convertHalf(v, from, to) : nullptr;

    return lowering_.resultLowerer().makeResult(result, lowering_.builder_.getInt1(ok),
                                                ok ? half : nullptr, ok ? nullptr : half);
  };
  // What a callback of andThen, orElse or match gave, of type `given`: a
  // promise to follow, or the value.
  auto settleWith = [&](llvm::Value *v, const Type *given) {
    if (given->isPromise() && given->promised() == result) {
      lowering_.promiseLowerer().followPromise(settled, v);
    } else if (given->isPromise()) {
      // Its value converts on the way, which takes the turn of the queue
      // that following it would.
      lowering_.reactionLowerer().attachReaction(
          v, settled,
          lowering_.reactionLowerer().reactionHandler(Builtin::PromiseThen, given->promised(),
                                                      result, nullptr, nullptr),
          nullptr, nullptr, /*later=*/true);
      lowering_.refCounts().releaseValue(v, given);
    } else {
      lowering_.promiseLowerer().fulfillPromise(
          settled,
          holds && !given->isVoid() ? lowering_.exprLowerer().emitConversion(v, given, result)
                                    : nullptr,
          result);
    }
    done();
  };

  llvm::Value *state = lowering_.builder_.CreateLoad(
      lowering_.builder_.getInt32Ty(), lowering_.promiseLowerer().promiseField(source, kState),
      "state");
  llvm::BasicBlock *fulfilledBlock = lowering_.functionFrame().newBlock("fulfilled");
  llvm::BasicBlock *rejectedBlock = lowering_.functionFrame().newBlock("rejected");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpEQ(state, lowering_.builder_.getInt32(kFulfilled)),
      fulfilledBlock, rejectedBlock);

  // Rejected: the error goes on, unless the code catches it.
  lowering_.functionFrame().switchTo(rejectedBlock);
  llvm::Value *error = lowering_.builder_.CreateLoad(
      ptr, lowering_.promiseLowerer().promiseField(source, kError), "error");
  lowering_.groupLowerer().retainObject(error);
  switch (kind) {
  case Builtin::ResultAsyncFromPromise:
  case Builtin::ResultAsyncFromThrowable:
    // An Err of what toError makes of the error, or of the error.
    if (first) {
      llvm::Value *x = callWith(callback(kReactionFulfilled), first, error, errorType);
      threw(first, reject);
      lowering_.promiseLowerer().fulfillPromise(derived, made(false, x, first->ret()), result);
    } else {
      lowering_.promiseLowerer().fulfillPromise(derived, made(false, error, errorType), result);
    }
    break;
  case Builtin::ResultAsyncTeeDone:
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {error}); // a tee does not care
    lowering_.promiseLowerer().fulfillPromise(derived, nullptr,
                                              result); // the Result is in the value already
    break;
  case Builtin::ResultAsyncUnwrapOr:
    if (holds) {
      // The default waits in the value: it goes.
      llvm::Value *slot = lowering_.promiseLowerer().promiseValueAddress(derived);
      lowering_.refCounts().releaseValue(
          lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(result), slot), result);
      lowering_.builder_.CreateStore(
          llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(result)), slot);
    }
    lowering_.promiseLowerer().rejectPromise(derived, error);
    break;
  default:
    lowering_.promiseLowerer().rejectPromise(derived, error);
    break;
  }
  lowering_.builder_.CreateRetVoid();

  // Fulfilled.
  lowering_.functionFrame().switchTo(fulfilledBlock);
  llvm::Value *given = nullptr;
  if (lowering_.promiseLowerer().promiseValueSize(value)) {
    given = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(value),
                                          lowering_.promiseLowerer().promiseValueAddress(source),
                                          "value");
    lowering_.refCounts().retainValue(given, value);
  }
  switch (kind) {
  case Builtin::ResultAsyncFromPromise: // its catch(): the Result goes on
    lowering_.promiseLowerer().fulfillPromise(derived, given, result);
    lowering_.builder_.CreateRetVoid();
    break;
  case Builtin::ResultAsyncFromSafePromise:
  case Builtin::ResultAsyncFromThrowable:
  case Builtin::ResultAsyncWrapOk:
  case Builtin::ResultAsyncWrapErr:
    lowering_.promiseLowerer().fulfillPromise(
        derived, made(kind != Builtin::ResultAsyncWrapErr, given, value), result);
    lowering_.builder_.CreateRetVoid();
    break;
  case Builtin::ResultAsyncTeeDone:
    if (given) {
      lowering_.refCounts().releaseValue(given, value);
    }
    lowering_.promiseLowerer().fulfillPromise(derived, nullptr,
                                              result); // the Result is in the value already
    lowering_.builder_.CreateRetVoid();
    break;
  default: {
    // A Result came: the method goes by its half.
    if (kind == Builtin::ResultAsyncMap || kind == Builtin::ResultAsyncMapErr ||
        kind == Builtin::ResultAsyncOrElse || kind == Builtin::ResultAsyncAndTee ||
        kind == Builtin::ResultAsyncOrTee) {
      settled = lowering_.promiseLowerer().newPromise(result);
    }
    const Type *okType = value->okType(), *errType = value->errType();
    llvm::Value *res =
        given ? given : llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(value));
    llvm::Value *okHalf = lowering_.resultLowerer().resultHalf(res, value, false),
                *errHalf = lowering_.resultLowerer().resultHalf(res, value, true);
    llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("ok");
    llvm::BasicBlock *errBlock = lowering_.functionFrame().newBlock("err");
    lowering_.builder_.CreateCondBr(lowering_.resultLowerer().resultIsOk(res, value), okBlock,
                                    errBlock);
    bool onError = kind == Builtin::ResultAsyncMapErr || kind == Builtin::ResultAsyncOrElse ||
                   kind == Builtin::ResultAsyncOrTee;
    bool tee = kind == Builtin::ResultAsyncAndTee || kind == Builtin::ResultAsyncOrTee;
    for (bool isOk : {true, false}) {
      lowering_.functionFrame().switchTo(isOk ? okBlock : errBlock);
      llvm::Value *half = isOk ? okHalf : errHalf;
      const Type *halfType = isOk ? okType : errType;
      if (halfType->isNever()) { // a Result without this half
        lowering_.builder_.CreateUnreachable();
        continue;
      }
      if (kind == Builtin::ResultAsyncUnwrapOr) {
        if (isOk && holds) {
          // The value instead of the default.
          llvm::Value *slot = lowering_.promiseLowerer().promiseValueAddress(derived);
          lowering_.refCounts().releaseValue(
              lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(result), slot), result);
          lowering_.builder_.CreateStore(
              llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(result)), slot);
          lowering_.refCounts().retainValue(half, halfType);
          lowering_.refCounts().releaseValue(res, value);
          lowering_.promiseLowerer().fulfillPromise(
              derived, lowering_.exprLowerer().emitConversion(half, halfType, result), result);
        } else {
          lowering_.refCounts().releaseValue(res, value);
          lowering_.promiseLowerer().fulfillPromise(derived, nullptr, result); // the default
        }
        lowering_.builder_.CreateRetVoid();
        continue;
      }
      if (kind != Builtin::ResultAsyncMatch && isOk == onError) {
        // The half nothing is called for goes on as it is.
        if (tee) {
          lowering_.promiseLowerer().fulfillPromise(settled, res, result);
        } else {
          llvm::Value *keep = halfType->isVoid() ? nullptr : half;
          lowering_.promiseLowerer().fulfillPromise(settled, made(isOk, keep, halfType), result);
        }
        done();
        continue;
      }
      bool onSecond = kind == Builtin::ResultAsyncMatch && !isOk;
      const Type *fnType = onSecond ? second : first;
      if (tee) {
        // The Result waits in the value until the callback is done.
        lowering_.refCounts().retainValue(half, halfType);
        if (holds) {
          lowering_.builder_.CreateStore(res,
                                         lowering_.promiseLowerer().promiseValueAddress(settled));
        } else {
          lowering_.refCounts().releaseValue(res, value);
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
          lowering_.reactionLowerer().attachReaction(
              x, settled,
              resultAsyncHandler(isOk ? Builtin::ResultAsyncWrapOk : Builtin::ResultAsyncWrapErr,
                                 ret->promised(), result, nullptr, nullptr),
              nullptr, nullptr);
          lowering_.refCounts().releaseValue(x, ret);
        } else {
          if (holds) {
            lowering_.builder_.CreateStore(made(isOk, ret->isVoid() ? nullptr : x, ret),
                                           lowering_.promiseLowerer().promiseValueAddress(settled));
          }
          lowering_.builder_.CreateCall(fulfillLater, {settled});
        }
        done();
        break;
      case Builtin::ResultAsyncAndTee:
      case Builtin::ResultAsyncOrTee:
        // `try { await f(value) } catch {}`, then the Result: when f
        // throws, the error goes and the Result goes on now.
        threw(fnType, [&](llvm::Value *thrown) {
          lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {thrown});
          lowering_.promiseLowerer().fulfillPromise(settled, nullptr, result);
        });
        if (ret->isPromise()) {
          lowering_.reactionLowerer().attachReaction(x, settled,
                                                     resultAsyncHandler(Builtin::ResultAsyncTeeDone,
                                                                        ret->promised(), result,
                                                                        nullptr, nullptr),
                                                     nullptr, nullptr);
          lowering_.refCounts().releaseValue(x, ret);
        } else {
          if (x && !ret->isVoid()) {
            lowering_.refCounts().releaseValue(x, ret);
          }
          lowering_.builder_.CreateCall(fulfillLater, {settled});
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
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return handler;
}

} // namespace tz
