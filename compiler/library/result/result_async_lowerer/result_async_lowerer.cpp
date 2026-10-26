#include "compiler/library/result/result_async_lowerer/result_async_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/result/result_async_handler/result_async_handler.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

ResultAsyncLowerer::ResultAsyncLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *ResultAsyncLowerer::emitResultAsync(CallExpr *call) {
  if (call->builtin == Builtin::ResultAsyncFromThrowable) {
    return emitResultAsyncThrowable(call);
  }
  const Type *result = call->type->promised();
  bool fromPromise = call->builtin == Builtin::ResultAsyncFromPromise ||
                     call->builtin == Builtin::ResultAsyncFromSafePromise;
  Expr *sourceExpr = fromPromise ? call->args[0] : cast<MemberExpr>(call->callee)->object;
  std::vector<Expr *> exprs{sourceExpr};
  for (size_t i = fromPromise ? 1 : 0; i < call->args.size(); ++i) {
    exprs.push_back(call->args[i]);
  }
  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(exprs);
  const Type *sourceType = sourceExpr->type;
  if (sourceType->isResult()) {
    return emitResultAsyncOf(call, values);
  }
  llvm::Value *derived = lowering_.promiseLowerer().newPromise(result);
  llvm::Value *callbacks[2] = {nullptr, nullptr};
  const Type *types[2] = {nullptr, nullptr};
  if (call->builtin == Builtin::ResultAsyncUnwrapOr) {
    if (lowering_.promiseLowerer().promiseValueSize(result)) {
      lowering_.builder_.CreateStore(values[1],
                                     lowering_.promiseLowerer().promiseValueAddress(derived));
    }
  } else {
    for (size_t i = 1; i < exprs.size() && i <= 2; ++i) {
      callbacks[i - 1] = values[i];
      types[i - 1] = exprs[i]->type;
    }
  }
  llvm::Value *source = values[0];
  if (call->builtin == Builtin::ResultAsyncFromPromise) {
    llvm::Value *step = lowering_.promiseLowerer().newPromise(result);
    lowering_.reactionLowerer().attachReaction(
        source, step,
        lowering_.resultAsyncHandler().resultAsyncHandler(
            Builtin::ResultAsyncFromSafePromise, sourceType->promised(), result, nullptr, nullptr),
        nullptr, nullptr);
    lowering_.reactionLowerer().attachReaction(
        step, derived,
        lowering_.resultAsyncHandler().resultAsyncHandler(Builtin::ResultAsyncFromPromise, result,
                                                          result, types[0], nullptr),
        callbacks[0], nullptr);
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {step});
  } else {
    lowering_.reactionLowerer().attachReaction(
        source, derived,
        lowering_.resultAsyncHandler().resultAsyncHandler(call->builtin, sourceType->promised(),
                                                          result, types[0], types[1]),
        callbacks[0], callbacks[1]);
  }
  lowering_.refCounts().releaseValue(source, sourceType);

  return derived;
}

llvm::Value *ResultAsyncLowerer::emitResultAsyncOf(CallExpr *call,
                                                   llvm::ArrayRef<llvm::Value *> values) {
  const Type *result = call->type->promised();
  const Type *type = cast<MemberExpr>(call->callee)->object->type;
  const Type *fnType = call->args[0]->type, *ret = fnType->ret();
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *res = values[0], *fn = values[1];
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("ok");
  llvm::BasicBlock *errBlock = lowering_.functionFrame().newBlock("err");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("done");
  lowering_.builder_.CreateCondBr(lowering_.resultLowerer().resultIsOk(res, type), okBlock,
                                  errBlock);

  lowering_.functionFrame().switchTo(errBlock);
  const Type *errType = type->errType();
  llvm::Value *failed = lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_promise_resolved", ptr, {ptr}),
      {lowering_.promiseLowerer().promiseLayout(result)}, "promise");
  llvm::Value *half =
      errType->isVoid() || errType->isNever()
          ? nullptr
          : lowering_.resultLowerer().convertHalf(
                lowering_.resultLowerer().resultHalf(res, type, true), errType, result->errType());
  lowering_.builder_.CreateStore(
      lowering_.resultLowerer().makeResult(result, lowering_.builder_.getFalse(), nullptr, half),
      lowering_.promiseLowerer().promiseValueAddress(failed));
  lowering_.refCounts().releaseValue(fn, fnType);
  llvm::BasicBlock *errEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(doneBlock);

  lowering_.functionFrame().switchTo(okBlock);
  std::vector<llvm::Value *> args;
  const Type *okType = type->okType();
  llvm::Value *v = lowering_.resultLowerer().resultHalf(res, type, false);
  if (fnType->params().empty()) {
    if (!okType->isVoid() && !okType->isNever()) {
      lowering_.refCounts().releaseValue(v, okType);
    }
  } else {
    const Type *param = fnType->params()[0];
    const Type *given = okType->isVoid() ? Type::undefinedTy() : okType;
    llvm::Value *arg = okType->isVoid() ? lowering_.builder_.getFalse() : v;
    args.push_back(given == param ? arg
                                  : lowering_.exprLowerer().emitConversion(arg, given, param));
  }
  llvm::Value *gave = lowering_.closureLowerer().callFunctionValue(fn, fnType, args);
  lowering_.refCounts().releaseValue(fn, fnType);
  lowering_.emitThrowCheckIf(lowering_.typeThrows(fnType));
  llvm::Value *made = gave;
  if (call->builtin == Builtin::ResultAsyncMap || ret->promised() != result) {
    made = lowering_.promiseLowerer().newPromise(result);
    lowering_.reactionLowerer().attachReaction(
        gave, made,
        call->builtin == Builtin::ResultAsyncMap
            ? lowering_.resultAsyncHandler().resultAsyncHandler(
                  Builtin::ResultAsyncFromSafePromise, ret->promised(), result, nullptr, nullptr)
            : lowering_.reactionLowerer().reactionHandler(Builtin::PromiseThen, ret->promised(),
                                                          result, nullptr, nullptr),
        nullptr, nullptr);
    lowering_.refCounts().releaseValue(gave, ret);
  }
  llvm::BasicBlock *okEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(doneBlock);

  lowering_.functionFrame().switchTo(doneBlock);
  llvm::PHINode *promise = lowering_.builder_.CreatePHI(ptr, 2, "result.async");
  promise->addIncoming(failed, errEnd);
  promise->addIncoming(made, okEnd);

  return promise;
}

llvm::Value *ResultAsyncLowerer::emitResultAsyncThrowable(CallExpr *call) {
  const Type *fnType = call->args[0]->type;
  const Type *toErrorType = call->args.size() > 1 ? call->args[1]->type : nullptr;
  const Type *madeType = call->type;
  const Type *result = madeType->ret()->promised();
  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(call->args);
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  auto *envType =
      llvm::StructType::get(lowering_.ctx_, {i64, ptr, lowering_.fnTy_, lowering_.fnTy_});
  auto *dropType = llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr}, false);
  auto *drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromAsyncThrowable.env.drop", lowering_.module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", drop));
  for (unsigned i : {2u, 3u}) {
    llvm::Value *fn = lowering_.builder_.CreateLoad(
        lowering_.fnTy_, lowering_.builder_.CreateStructGEP(envType, drop->getArg(0), i));
    lowering_.builder_.CreateCall(lowering_.objReleaseFn_,
                                  {lowering_.builder_.CreateExtractValue(fn, 1)});
  }
  lowering_.builder_.CreateCall(lowering_.rt_.objFree, {drop->getArg(0)});
  lowering_.builder_.CreateRetVoid();

  llvm::FunctionType *codeType = lowering_.closureLowerer().closureFnType(madeType);
  auto *code = llvm::Function::Create(codeType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromAsyncThrowable", lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(code);
  llvm::Function *outer = std::exchange(lowering_.function_, code);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", code));
  llvm::Value *env = code->getArg(0);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < code->arg_size(); ++i) {
    args.push_back(code->getArg(i));
  }
  llvm::Value *f = lowering_.builder_.CreateLoad(
      lowering_.fnTy_, lowering_.builder_.CreateStructGEP(envType, env, 2), "f");
  llvm::Value *toError = lowering_.builder_.CreateLoad(
      lowering_.fnTy_, lowering_.builder_.CreateStructGEP(envType, env, 3), "toError");
  llvm::Value *promise = lowering_.closureLowerer().callFunctionValue(f, fnType, args);
  llvm::Value *derived = lowering_.promiseLowerer().newPromise(result);
  if (lowering_.typeThrows(fnType)) {
    llvm::Value *thrown =
        lowering_.builder_.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot(), "thrown");
    llvm::BasicBlock *threwBlock = lowering_.functionFrame().newBlock("threw");
    llvm::BasicBlock *gaveBlock = lowering_.functionFrame().newBlock("gave");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(thrown), threwBlock,
                                    gaveBlock,
                                    llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
    lowering_.functionFrame().switchTo(threwBlock);
    lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr),
                                   lowering_.throwLowerer().thrownSlot());
    llvm::Value *errorHalf = thrown;
    const Type *errorType = lowering_.errorClass_->type;
    if (toErrorType) {
      std::vector<llvm::Value *> errorArgs;
      if (!toErrorType->params().empty()) {
        errorArgs.push_back(thrown);
      } else {
        lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {thrown});
      }
      errorHalf = lowering_.closureLowerer().callFunctionValue(toError, toErrorType, errorArgs);
      errorType = toErrorType->ret();
      if (lowering_.typeThrows(toErrorType)) {
        llvm::Value *again =
            lowering_.builder_.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot(), "thrown");
        llvm::BasicBlock *failedBlock = lowering_.functionFrame().newBlock("failed");
        llvm::BasicBlock *madeBlock = lowering_.functionFrame().newBlock("made");
        lowering_.builder_.CreateCondBr(
            lowering_.builder_.CreateIsNotNull(again), failedBlock, madeBlock,
            llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
        lowering_.functionFrame().switchTo(failedBlock);
        lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr),
                                       lowering_.throwLowerer().thrownSlot());
        lowering_.promiseLowerer().rejectPromise(derived, again);
        lowering_.builder_.CreateRet(derived);
        lowering_.functionFrame().switchTo(madeBlock);
      }
    }
    lowering_.promiseLowerer().fulfillPromise(
        derived,
        lowering_.resultLowerer().makeResult(
            result, lowering_.builder_.getFalse(), nullptr,
            lowering_.resultLowerer().convertHalf(errorHalf, errorType, result->errType())),
        result);
    lowering_.builder_.CreateRet(derived);
    lowering_.functionFrame().switchTo(gaveBlock);
  }
  if (toErrorType) {
    lowering_.builder_.CreateCall(lowering_.objRetainFn_,
                                  {lowering_.builder_.CreateExtractValue(toError, 1)});
  }
  lowering_.reactionLowerer().attachReaction(
      promise, derived,
      lowering_.resultAsyncHandler().resultAsyncHandler(Builtin::ResultAsyncFromThrowable,
                                                        fnType->ret()->promised(), result,
                                                        toErrorType, nullptr),
      toErrorType ? toError : nullptr, nullptr);
  lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {promise});
  lowering_.builder_.CreateRet(derived);
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  auto *info = new llvm::GlobalVariable(
      *lowering_.module_, lowering_.fnInfoTy_, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(lowering_.fnInfoTy_,
                                {code, lowering_.stringLowerer().stringLiteral(u"")}),
      "tz.fninfo.fromAsyncThrowable");
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(envType);
  llvm::Value *object = lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                                      {lowering_.builder_.getInt64(size)}, "env");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                 lowering_.builder_.CreateStructGEP(envType, object, 0));
  lowering_.builder_.CreateStore(lowering_.classDescriptors().dropDescriptor(drop),
                                 lowering_.builder_.CreateStructGEP(envType, object, 1));
  lowering_.builder_.CreateStore(values[0], lowering_.builder_.CreateStructGEP(envType, object, 2));
  lowering_.builder_.CreateStore(values.size() > 1 ? values[1]
                                                   : llvm::Constant::getNullValue(lowering_.fnTy_),
                                 lowering_.builder_.CreateStructGEP(envType, object, 3));
  if (lowering_.parallel_) {
    const Type *anyFn = Type::function({}, 0, false, Type::voidTy());
    lowering_.publishedObjects_.push_back(
        {lowering_.classDescriptors().dropDescriptor(drop), envType, {{2u, anyFn}, {3u, anyFn}}});
  }

  return lowering_.closureLowerer().makeFunctionValue(info, object);
}

}
