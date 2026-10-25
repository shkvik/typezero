#include "compiler/library/result/from_throwable_lowerer/from_throwable_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

FromThrowableLowerer::FromThrowableLowerer(Lowering &lowering) : lowering_(lowering) {}

// A function value whose environment holds f and toError: its code calls f
// with its arguments and makes a Result of what f returns, or of the error
// f throws, taken from tz_thrown (and given to toError).
llvm::Value *FromThrowableLowerer::emitFromThrowable(CallExpr *call) {
  const Type *fnType = call->args[0]->type;
  const Type *toErrorType = call->args.size() > 1 ? call->args[1]->type : nullptr;
  const Type *madeType = call->type, *resultType = madeType->ret();
  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(call->args);
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  auto *envType =
      llvm::StructType::get(lowering_.ctx_, {i64, ptr, lowering_.fnTy_, lowering_.fnTy_});

  // The environment, which holds both functions (a null one without
  // toError) and releases them when it is dropped.
  auto *dropType = llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr}, false);
  auto *drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromThrowable.env.drop", lowering_.module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", drop));
  for (unsigned i : {2u, 3u}) {
    lowering_.refCounts().releaseValue(
        lowering_.builder_.CreateLoad(
            lowering_.fnTy_, lowering_.builder_.CreateStructGEP(envType, drop->getArg(0), i)),
        fnType);
  }
  lowering_.builder_.CreateCall(lowering_.rt_.objFree, {drop->getArg(0)});
  lowering_.builder_.CreateRetVoid();

  // The code.
  llvm::FunctionType *codeType = lowering_.closureLowerer().closureFnType(madeType);
  auto *code = llvm::Function::Create(codeType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromThrowable", lowering_.module_.get());
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
  llvm::Value *returned = lowering_.closureLowerer().callFunctionValue(f, fnType, args);
  llvm::Value *thrown =
      lowering_.builder_.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot(), "thrown");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("ok");
  llvm::BasicBlock *errBlock = lowering_.functionFrame().newBlock("err");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNull(thrown), okBlock, errBlock);
  lowering_.functionFrame().switchTo(okBlock);
  const Type *value = fnType->ret();
  lowering_.builder_.CreateRet(lowering_.resultLowerer().makeResult(
      resultType, lowering_.builder_.getTrue(),
      lowering_.resultLowerer().convertHalf(value->isVoid() ? nullptr : returned, value,
                                            resultType->okType()),
      nullptr));
  lowering_.functionFrame().switchTo(errBlock);
  lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr),
                                 lowering_.throwLowerer().thrownSlot());
  llvm::Value *error = thrown;
  if (toErrorType) {
    llvm::Value *toError = lowering_.builder_.CreateLoad(
        lowering_.fnTy_, lowering_.builder_.CreateStructGEP(envType, env, 3), "toError");
    std::vector<llvm::Value *> errorArgs;
    if (!toErrorType->params().empty()) {
      errorArgs.push_back(thrown);
    } else {
      lowering_.refCounts().releaseValue(thrown, lowering_.errorClass_->type);
    }
    error = lowering_.closureLowerer().callFunctionValue(toError, toErrorType, errorArgs);
    // toError threw in turn: that goes to the caller.
    if (lowering_.typeThrows(toErrorType)) {
      llvm::BasicBlock *madeBlock = lowering_.functionFrame().newBlock("made");
      llvm::BasicBlock *failedBlock = lowering_.functionFrame().newBlock("failed");
      lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNull(lowering_.builder_.CreateLoad(
                                          ptr, lowering_.throwLowerer().thrownSlot())),
                                      madeBlock, failedBlock);
      lowering_.functionFrame().switchTo(failedBlock);
      lowering_.builder_.CreateRet(
          llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(resultType)));
      lowering_.functionFrame().switchTo(madeBlock);
    }
  }
  lowering_.builder_.CreateRet(lowering_.resultLowerer().makeResult(
      resultType, lowering_.builder_.getFalse(), nullptr, error));
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  auto *info = new llvm::GlobalVariable(
      *lowering_.module_, lowering_.fnInfoTy_, /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(lowering_.fnInfoTy_,
                                {code, lowering_.stringLowerer().stringLiteral(u"")}),
      "tz.fninfo.fromThrowable");
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

// The half asked for, or an Error thrown for the other one, as neverthrow's
// messages say.
llvm::Value *FromThrowableLowerer::emitUnsafeUnwrap(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  bool wantErr = call->builtin == Builtin::ResultUnsafeUnwrapErr;
  Lowering::Operand result = lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *ok = lowering_.resultLowerer().resultIsOk(result.value, type);
  llvm::BasicBlock *goodBlock = lowering_.functionFrame().newBlock("unwrap");
  llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("unwrap.other");
  lowering_.builder_.CreateCondBr(wantErr ? lowering_.builder_.CreateNot(ok) : ok, goodBlock,
                                  badBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createLikelyBranchWeights());
  lowering_.functionFrame().switchTo(badBlock);
  lowering_.refCounts().dropOperand(result);
  lowering_.throwLowerer().emitThrowNew(0, wantErr ? "Called `_unsafeUnwrapErr` on an Ok"
                                                   : "Called `_unsafeUnwrap` on an Err");
  lowering_.functionFrame().switchTo(goodBlock);
  const Type *half = wantErr ? type->errType() : type->okType();
  llvm::Value *value = nullptr;
  if (!half->isVoid() && !half->isNever()) {
    value = lowering_.resultLowerer().resultHalf(result.value, type, wantErr);
    lowering_.refCounts().retainValue(value, half);
  }
  lowering_.refCounts().dropOperand(result);
  if (call->type->isUndefined() || !value) {
    return value ? value
                 : llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(call->type));
  }

  return lowering_.exprLowerer().emitConversion(value, half, call->type);
}

} // namespace tz
