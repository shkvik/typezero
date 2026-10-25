#include "compiler/library/result/from_throwable_lowerer/from_throwable_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

FromThrowableLowerer::FromThrowableLowerer(CodeGen &codegen) : codegen_(codegen) {}

// A function value whose environment holds f and toError: its code calls f
// with its arguments and makes a Result of what f returns, or of the error
// f throws, taken from tz_thrown (and given to toError).
llvm::Value *FromThrowableLowerer::emitFromThrowable(CallExpr *call) {
  const Type *fnType = call->args[0]->type;
  const Type *toErrorType = call->args.size() > 1 ? call->args[1]->type : nullptr;
  const Type *madeType = call->type, *resultType = madeType->ret();
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i64 = codegen_.builder_.getInt64Ty();
  auto *envType = llvm::StructType::get(codegen_.ctx_, {i64, ptr, codegen_.fnTy_, codegen_.fnTy_});

  // The environment, which holds both functions (a null one without
  // toError) and releases them when it is dropped.
  auto *dropType = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr}, false);
  auto *drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromThrowable.env.drop", codegen_.module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", drop));
  for (unsigned i : {2u, 3u}) {
    codegen_.refCounts().releaseValue(codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(envType, drop->getArg(0), i)),
                 fnType);
  }
  codegen_.builder_.CreateCall(codegen_.rt_.objFree, {drop->getArg(0)});
  codegen_.builder_.CreateRetVoid();

  // The code.
  llvm::FunctionType *codeType = codegen_.closureLowerer().closureFnType(madeType);
  auto *code = llvm::Function::Create(codeType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromThrowable", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(code);
  llvm::Function *outer = std::exchange(codegen_.function_, code);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
  llvm::Value *env = code->getArg(0);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < code->arg_size(); ++i) {
    args.push_back(code->getArg(i));
  }
  llvm::Value *f = codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(envType, env, 2), "f");
  llvm::Value *returned = codegen_.closureLowerer().callFunctionValue(f, fnType, args);
  llvm::Value *thrown = codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot(), "thrown");
  llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("ok");
  llvm::BasicBlock *errBlock = codegen_.functionFrame().newBlock("err");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNull(thrown), okBlock, errBlock);
  codegen_.functionFrame().switchTo(okBlock);
  const Type *value = fnType->ret();
  codegen_.builder_.CreateRet(codegen_.resultLowerer().makeResult(
      resultType, codegen_.builder_.getTrue(),
      codegen_.resultLowerer().convertHalf(value->isVoid() ? nullptr : returned, value, resultType->okType()), nullptr));
  codegen_.functionFrame().switchTo(errBlock);
  codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), codegen_.throwLowerer().thrownSlot());
  llvm::Value *error = thrown;
  if (toErrorType) {
    llvm::Value *toError =
        codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.builder_.CreateStructGEP(envType, env, 3), "toError");
    std::vector<llvm::Value *> errorArgs;
    if (!toErrorType->params().empty()) {
      errorArgs.push_back(thrown);
    } else {
      codegen_.refCounts().releaseValue(thrown, codegen_.errorClass_->type);
    }
    error = codegen_.closureLowerer().callFunctionValue(toError, toErrorType, errorArgs);
    // toError threw in turn: that goes to the caller.
    if (codegen_.typeThrows(toErrorType)) {
      llvm::BasicBlock *madeBlock = codegen_.functionFrame().newBlock("made");
      llvm::BasicBlock *failedBlock = codegen_.functionFrame().newBlock("failed");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNull(codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot())),
                            madeBlock, failedBlock);
      codegen_.functionFrame().switchTo(failedBlock);
      codegen_.builder_.CreateRet(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(resultType)));
      codegen_.functionFrame().switchTo(madeBlock);
    }
  }
  codegen_.builder_.CreateRet(codegen_.resultLowerer().makeResult(resultType, codegen_.builder_.getFalse(), nullptr, error));
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);

  auto *info = new llvm::GlobalVariable(
      *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(codegen_.fnInfoTy_, {code, codegen_.stringLowerer().stringLiteral(u"")}), "tz.fninfo.fromThrowable");
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

// The half asked for, or an Error thrown for the other one, as neverthrow's
// messages say.
llvm::Value *FromThrowableLowerer::emitUnsafeUnwrap(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  bool wantErr = call->builtin == Builtin::ResultUnsafeUnwrapErr;
  CodeGen::Operand result = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *ok = codegen_.resultLowerer().resultIsOk(result.value, type);
  llvm::BasicBlock *goodBlock = codegen_.functionFrame().newBlock("unwrap");
  llvm::BasicBlock *badBlock = codegen_.functionFrame().newBlock("unwrap.other");
  codegen_.builder_.CreateCondBr(wantErr ? codegen_.builder_.CreateNot(ok) : ok, goodBlock, badBlock,
                        llvm::MDBuilder(codegen_.ctx_).createLikelyBranchWeights());
  codegen_.functionFrame().switchTo(badBlock);
  codegen_.refCounts().dropOperand(result);
  codegen_.throwLowerer().emitThrowNew(0,
               wantErr ? "Called `_unsafeUnwrapErr` on an Ok" : "Called `_unsafeUnwrap` on an Err");
  codegen_.functionFrame().switchTo(goodBlock);
  const Type *half = wantErr ? type->errType() : type->okType();
  llvm::Value *value = nullptr;
  if (!half->isVoid() && !half->isNever()) {
    value = codegen_.resultLowerer().resultHalf(result.value, type, wantErr);
    codegen_.refCounts().retainValue(value, half);
  }
  codegen_.refCounts().dropOperand(result);
  if (call->type->isUndefined() || !value) {
    return value ? value : llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(call->type));
  }

  return codegen_.exprLowerer().emitConversion(value, half, call->type);
}

} // namespace tz
