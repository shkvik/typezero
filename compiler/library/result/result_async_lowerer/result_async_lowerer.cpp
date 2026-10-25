#include "compiler/library/result/result_async_lowerer/result_async_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/library/result/result_async_handler/result_async_handler.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
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

ResultAsyncLowerer::ResultAsyncLowerer(CodeGen &codegen) : codegen_(codegen) {}

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
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(exprs);
  const Type *sourceType = sourceExpr->type;
  if (sourceType->isResult()) {
    return emitResultAsyncOf(call, values);
  }
  llvm::Value *derived = codegen_.promiseLowerer().newPromise(result);
  llvm::Value *callbacks[2] = {nullptr, nullptr};
  const Type *types[2] = {nullptr, nullptr};
  if (call->builtin == Builtin::ResultAsyncUnwrapOr) {
    // The default waits in the value of the promise until it is needed.
    if (codegen_.promiseLowerer().promiseValueSize(result)) {
      codegen_.builder_.CreateStore(values[1], codegen_.promiseLowerer().promiseValueAddress(derived));
    }
  } else {
    for (size_t i = 1; i < exprs.size() && i <= 2; ++i) {
      callbacks[i - 1] = values[i];
      types[i - 1] = exprs[i]->type;
    }
  }
  llvm::Value *source = values[0];
  if (call->builtin == Builtin::ResultAsyncFromPromise) {
    // `promise.then((v) => ok(v)).catch((e) => err(toError(e)))`.
    llvm::Value *step = codegen_.promiseLowerer().newPromise(result);
    codegen_.reactionLowerer().attachReaction(source, step,
                   codegen_.resultAsyncHandler().resultAsyncHandler(Builtin::ResultAsyncFromSafePromise, sourceType->promised(),
                                      result, nullptr, nullptr),
                   nullptr, nullptr);
    codegen_.reactionLowerer().attachReaction(
        step, derived,
        codegen_.resultAsyncHandler().resultAsyncHandler(Builtin::ResultAsyncFromPromise, result, result, types[0], nullptr),
        callbacks[0], nullptr);
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {step});
  } else {
    codegen_.reactionLowerer().attachReaction(
        source, derived,
        codegen_.resultAsyncHandler().resultAsyncHandler(call->builtin, sourceType->promised(), result, types[0], types[1]),
        callbacks[0], callbacks[1]);
  }
  codegen_.refCounts().releaseValue(source, sourceType);

  return derived;
}

// asyncMap and asyncAndThen of a Result, whose function runs now, as in
// neverthrow: an Ok gives `fromSafePromise(f(value))` and `f(value)`, an Err
// `errAsync(error)`.
llvm::Value *ResultAsyncLowerer::emitResultAsyncOf(CallExpr *call, llvm::ArrayRef<llvm::Value *> values) {
  const Type *result = call->type->promised();
  const Type *type = cast<MemberExpr>(call->callee)->object->type;
  const Type *fnType = call->args[0]->type, *ret = fnType->ret();
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *res = values[0], *fn = values[1];
  llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("ok");
  llvm::BasicBlock *errBlock = codegen_.functionFrame().newBlock("err");
  llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("done");
  codegen_.builder_.CreateCondBr(codegen_.resultLowerer().resultIsOk(res, type), okBlock, errBlock);

  codegen_.functionFrame().switchTo(errBlock);
  const Type *errType = type->errType();
  llvm::Value *failed = codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_promise_resolved", ptr, {ptr}),
                                            {codegen_.promiseLowerer().promiseLayout(result)}, "promise");
  llvm::Value *half = errType->isVoid() || errType->isNever()
                          ? nullptr
                          : codegen_.resultLowerer().convertHalf(codegen_.resultLowerer().resultHalf(res, type, true), errType, result->errType());
  codegen_.builder_.CreateStore(codegen_.resultLowerer().makeResult(result, codegen_.builder_.getFalse(), nullptr, half),
                       codegen_.promiseLowerer().promiseValueAddress(failed));
  codegen_.refCounts().releaseValue(fn, fnType);
  llvm::BasicBlock *errEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(doneBlock);

  codegen_.functionFrame().switchTo(okBlock);
  std::vector<llvm::Value *> args;
  const Type *okType = type->okType();
  llvm::Value *v = codegen_.resultLowerer().resultHalf(res, type, false);
  if (fnType->params().empty()) {
    if (!okType->isVoid() && !okType->isNever()) {
      codegen_.refCounts().releaseValue(v, okType);
    }
  } else {
    const Type *param = fnType->params()[0];
    const Type *given = okType->isVoid() ? Type::undefinedTy() : okType;
    llvm::Value *arg = okType->isVoid() ? codegen_.builder_.getFalse() : v;
    args.push_back(given == param ? arg : codegen_.exprLowerer().emitConversion(arg, given, param));
  }
  llvm::Value *gave = codegen_.closureLowerer().callFunctionValue(fn, fnType, args);
  codegen_.refCounts().releaseValue(fn, fnType);
  codegen_.emitThrowCheckIf(codegen_.typeThrows(fnType)); // it runs here, in the caller
  llvm::Value *made = gave;
  if (call->builtin == Builtin::ResultAsyncMap || ret->promised() != result) {
    made = codegen_.promiseLowerer().newPromise(result);
    codegen_.reactionLowerer().attachReaction(
        gave, made,
        call->builtin == Builtin::ResultAsyncMap
            ? codegen_.resultAsyncHandler().resultAsyncHandler(Builtin::ResultAsyncFromSafePromise, ret->promised(), result,
                                 nullptr, nullptr)
            : codegen_.reactionLowerer().reactionHandler(Builtin::PromiseThen, ret->promised(), result, nullptr, nullptr),
        nullptr, nullptr);
    codegen_.refCounts().releaseValue(gave, ret);
  }
  llvm::BasicBlock *okEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(doneBlock);

  codegen_.functionFrame().switchTo(doneBlock);
  llvm::PHINode *promise = codegen_.builder_.CreatePHI(ptr, 2, "result.async");
  promise->addIncoming(failed, errEnd);
  promise->addIncoming(made, okEnd);

  return promise;
}

// ResultAsync.fromThrowable(f, toError?): a function value whose
// environment holds f and toError. Its code is neverthrow's async function:
// it calls f and gives a ResultAsync of an Ok of what f's promise gives, or
// an Err of what f throws or the promise is rejected with, as toError makes
// it; once the promise settles, or now when f throws.
llvm::Value *ResultAsyncLowerer::emitResultAsyncThrowable(CallExpr *call) {
  const Type *fnType = call->args[0]->type;
  const Type *toErrorType = call->args.size() > 1 ? call->args[1]->type : nullptr;
  const Type *madeType = call->type;
  const Type *result = madeType->ret()->promised();
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i64 = codegen_.builder_.getInt64Ty();
  auto *envType = llvm::StructType::get(codegen_.ctx_, {i64, ptr, codegen_.fnTy_, codegen_.fnTy_});
  auto *dropType = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr}, false);
  auto *drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromAsyncThrowable.env.drop", codegen_.module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", drop));
  for (unsigned i : {2u, 3u}) {
    llvm::Value *fn =
        codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(envType, drop->getArg(0), i));
    codegen_.builder_.CreateCall(codegen_.objReleaseFn_, {codegen_.builder_.CreateExtractValue(fn, 1)});
  }
  codegen_.builder_.CreateCall(codegen_.rt_.objFree, {drop->getArg(0)});
  codegen_.builder_.CreateRetVoid();

  llvm::FunctionType *codeType = codegen_.closureLowerer().closureFnType(madeType);
  auto *code = llvm::Function::Create(codeType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromAsyncThrowable", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(code);
  llvm::Function *outer = std::exchange(codegen_.function_, code);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
  llvm::Value *env = code->getArg(0);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < code->arg_size(); ++i) {
    args.push_back(code->getArg(i));
  }
  llvm::Value *f = codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(envType, env, 2), "f");
  llvm::Value *toError =
      codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(envType, env, 3), "toError");
  llvm::Value *promise = codegen_.closureLowerer().callFunctionValue(f, fnType, args);
  llvm::Value *derived = codegen_.promiseLowerer().newPromise(result);
  if (codegen_.typeThrows(fnType)) {
    // f threw rather than give a promise: an Err of that now.
    llvm::Value *thrown = codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot(), "thrown");
    llvm::BasicBlock *threwBlock = codegen_.functionFrame().newBlock("threw");
    llvm::BasicBlock *gaveBlock = codegen_.functionFrame().newBlock("gave");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(thrown), threwBlock, gaveBlock,
                          llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
    codegen_.functionFrame().switchTo(threwBlock);
    codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), codegen_.throwLowerer().thrownSlot());
    llvm::Value *errorHalf = thrown;
    const Type *errorType = codegen_.errorClass_->type;
    if (toErrorType) {
      std::vector<llvm::Value *> errorArgs;
      if (!toErrorType->params().empty()) {
        errorArgs.push_back(thrown);
      } else {
        codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {thrown});
      }
      errorHalf = codegen_.closureLowerer().callFunctionValue(toError, toErrorType, errorArgs);
      errorType = toErrorType->ret();
      if (codegen_.typeThrows(toErrorType)) {
        // toError threw in turn: that rejects the ResultAsync.
        llvm::Value *again = codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot(), "thrown");
        llvm::BasicBlock *failedBlock = codegen_.functionFrame().newBlock("failed");
        llvm::BasicBlock *madeBlock = codegen_.functionFrame().newBlock("made");
        codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(again), failedBlock, madeBlock,
                              llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
        codegen_.functionFrame().switchTo(failedBlock);
        codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), codegen_.throwLowerer().thrownSlot());
        codegen_.promiseLowerer().rejectPromise(derived, again);
        codegen_.builder_.CreateRet(derived);
        codegen_.functionFrame().switchTo(madeBlock);
      }
    }
    codegen_.promiseLowerer().fulfillPromise(derived,
                   codegen_.resultLowerer().makeResult(result, codegen_.builder_.getFalse(), nullptr,
                              codegen_.resultLowerer().convertHalf(errorHalf, errorType, result->errType())),
                   result);
    codegen_.builder_.CreateRet(derived);
    codegen_.functionFrame().switchTo(gaveBlock);
  }
  if (toErrorType) {
    codegen_.builder_.CreateCall(codegen_.objRetainFn_, {codegen_.builder_.CreateExtractValue(toError, 1)});
  }
  codegen_.reactionLowerer().attachReaction(promise, derived,
                 codegen_.resultAsyncHandler().resultAsyncHandler(Builtin::ResultAsyncFromThrowable, fnType->ret()->promised(),
                                    result, toErrorType, nullptr),
                 toErrorType ? toError : nullptr, nullptr);
  codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {promise});
  codegen_.builder_.CreateRet(derived);
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);

  auto *info = new llvm::GlobalVariable(
      *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(codegen_.fnInfoTy_, {code, codegen_.stringLowerer().stringLiteral(u"")}),
      "tz.fninfo.fromAsyncThrowable");
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(envType);
  llvm::Value *object = codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {codegen_.builder_.getInt64(size)}, "env");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), codegen_.builder_.CreateStructGEP(envType, object, 0));
  codegen_.builder_.CreateStore(codegen_.classDescriptors().dropDescriptor(drop), codegen_.builder_.CreateStructGEP(envType, object, 1));
  codegen_.builder_.CreateStore(values[0], codegen_.builder_.CreateStructGEP(envType, object, 2));
  codegen_.builder_.CreateStore(values.size() > 1 ? values[1] : llvm::Constant::getNullValue(codegen_.fnTy_),
                       codegen_.builder_.CreateStructGEP(envType, object, 3));
  if (codegen_.parallel_) {
    const Type *anyFn = Type::function({}, 0, false, Type::voidTy());
    codegen_.publishedObjects_.push_back({codegen_.classDescriptors().dropDescriptor(drop), envType, {{2u, anyFn}, {3u, anyFn}}});
  }

  return codegen_.closureLowerer().makeFunctionValue(info, object);
}

} // namespace tz
