#include "compiler/lower/values/callback_method_lowerer/callback_method_lowerer.h"
#include "compiler/library/bytes/bytes_callback_lowerer/bytes_callback_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

enum ArrayField : unsigned { kRefCount, kElementType, kData, kLength, kCapacity, kAlloc };

}

CallbackMethodLowerer::CallbackMethodLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *CallbackMethodLowerer::emitCallbackMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  if (member->object->type->isTypedArray()) {
    return lowering_.bytesCallbackLowerer().emitBytesCallbackMethod(call);
  }
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  const Type *callbackType = call->args[0]->type;
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  Builtin method = call->builtin;

  llvm::Value *array = lowering_.exprLowerer().emitExpr(member->object);
  llvm::Value *callback = lowering_.exprLowerer().emitExpr(call->args[0]);
  llvm::Value *init =
      call->args.size() > 1 ? lowering_.exprLowerer().emitExpr(call->args[1]) : nullptr;

  if (method == Builtin::ArrSortWith) {
    llvm::Value *context = lowering_.functionFrame().spill(callback, "compare");
    lowering_.builder_.CreateCall(lowering_.rt_.arrSortWith,
                                  {array, sortComparator(call), context});
    lowering_.refCounts().releaseValue(callback, callbackType);
    return array;
  }

  bool reduce = method == Builtin::ArrReduce || method == Builtin::ArrReduceRight;
  bool backward = method == Builtin::ArrReduceRight || method == Builtin::ArrFindLast ||
                  method == Builtin::ArrFindLastIndex;
  size_t passes = std::min(callbackType->params().size(), size_t(reduce ? 4 : 3));
  size_t valueArgs = reduce ? 2 : 1;

  llvm::Value *length = lowering_.arrayLowerer().arrayLength(array);
  llvm::Value *start = backward
                           ? lowering_.builder_.CreateSub(length, lowering_.builder_.getInt64(1))
                           : lowering_.builder_.getInt64(0);
  llvm::Value *step = lowering_.builder_.getInt64(backward ? -1 : 1);
  auto readElement = [&](llvm::Value *at) {
    return lowering_.builder_.CreateLoad(
        lowering_.moduleBuilder().lower(element),
        lowering_.arrayLowerer().elementAddress(lowering_.arrayLowerer().arrayData(array), element,
                                                at),
        "element");
  };

  llvm::Value *accSlot = nullptr;
  const Type *accType = reduce && !init ? element : call->type;
  if (reduce) {
    accSlot =
        lowering_.functionFrame().createStackSlot(lowering_.moduleBuilder().lower(accType), "acc");
    if (init) {
      lowering_.builder_.CreateStore(init, accSlot);
    } else {
      start = firstOfReduce(call, length, start, step, accSlot, accType, readElement);
    }
  }
  llvm::Value *out = nullptr;
  if (method == Builtin::ArrMap || method == Builtin::ArrFilter || method == Builtin::ArrFlatMap) {
    out = lowering_.groupLowerer().newArray(
        call->type, lowering_.builder_.getInt64(0),
        method == Builtin::ArrMap ? length : lowering_.builder_.getInt64(0),
        lowering_.makesMember(call, call->type), "result");
  }
  if (out) {
    lowering_.groupLowerer().noteUnstored(out, call);
  }
  llvm::Value *resultSlot = nullptr;
  if (!reduce && !out && method != Builtin::ArrForEach) {
    resultSlot = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(call->type), "result");
    llvm::Value *none = method == Builtin::ArrSome    ? lowering_.builder_.getFalse()
                        : method == Builtin::ArrEvery ? lowering_.builder_.getTrue()
                        : call->type->isInt()
                            ? lowering_.builder_.getInt64(~uint64_t(0))
                            : lowering_.optionalLowerer().undefinedValue(call->type);
    lowering_.builder_.CreateStore(none, resultSlot);
  }

  llvm::Value *indexSlot = lowering_.functionFrame().createStackSlot(i64, "index");
  lowering_.builder_.CreateStore(start, indexSlot);
  llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("each");
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("each.body");
  llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("each.next");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("each.end");
  lowering_.functionFrame().emitBlock(headBlock);
  llvm::Value *i = lowering_.builder_.CreateLoad(i64, indexSlot, "i");
  llvm::Value *inRange =
      lowering_.builder_.CreateICmpULT(i, lowering_.arrayLowerer().arrayLength(array));
  if (!backward) {
    inRange = lowering_.builder_.CreateAnd(inRange, lowering_.builder_.CreateICmpULT(i, length));
  }
  lowering_.builder_.CreateCondBr(inRange, bodyBlock, endBlock);

  lowering_.functionFrame().switchTo(bodyBlock);
  llvm::Value *value = readElement(i);
  bool keeps =
      method == Builtin::ArrFilter || method == Builtin::ArrFind || method == Builtin::ArrFindLast;
  if (passes >= valueArgs) {
    lowering_.refCounts().retainValue(value, element);
  }
  if (keeps) {
    lowering_.refCounts().retainValue(value, element);
  }
  std::vector<llvm::Value *> args;
  if (reduce) {
    args.push_back(
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(accType), accSlot));
  }
  args.push_back(value);
  args.push_back(i);
  args.push_back(array);
  if (reduce && passes == 0) {
    lowering_.refCounts().releaseValue(args[0], accType);
  }
  args.resize(passes);
  if (passes == valueArgs + 2) {
    lowering_.refCounts().retainValue(array, arrayType);
  }
  llvm::Value *result = lowering_.closureLowerer().callFunctionValue(callback, callbackType, args);
  const Type *resultType = callbackType->ret();

  llvm::BasicBlock *foundBlock = nullptr;
  auto test = [&](llvm::Value *decides) {
    foundBlock = lowering_.functionFrame().newBlock("each.found");
    llvm::BasicBlock *otherBlock = lowering_.functionFrame().newBlock("each.other");
    lowering_.builder_.CreateCondBr(decides, foundBlock, otherBlock);
    lowering_.functionFrame().switchTo(otherBlock);
  };
  switch (method) {
  case Builtin::ArrForEach:
    if (result) {
      lowering_.refCounts().releaseValue(result, resultType);
    }
    break;
  case Builtin::ArrMap:
    lowering_.builder_.CreateStore(
        result, lowering_.arrayLowerer().elementAddress(lowering_.arrayLowerer().arrayData(out),
                                                        resultType, i));
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateTrunc(
            lowering_.builder_.CreateAdd(i, lowering_.builder_.getInt64(1)),
            lowering_.builder_.getInt32Ty()),
        lowering_.arrayLowerer().arrayField(out, kLength));
    break;
  case Builtin::ArrFlatMap:
    if (resultType->isArray()) {
      lowering_.builder_.CreateCall(lowering_.rt_.arrAppend, {out, result});
      lowering_.refCounts().releaseValue(result, resultType);
    } else {
      lowering_.arrayLowerer().emitPush(out, {result}, resultType);
    }
    break;
  case Builtin::ArrFilter: {
    llvm::BasicBlock *keepBlock = lowering_.functionFrame().newBlock("filter.keep");
    llvm::BasicBlock *dropBlock = lowering_.functionFrame().newBlock("filter.drop");
    lowering_.builder_.CreateCondBr(result, keepBlock, dropBlock);
    lowering_.functionFrame().switchTo(keepBlock);
    const Type *kept = call->type->inner();
    lowering_.arrayLowerer().emitPush(
        out, {lowering_.exprLowerer().emitConversion(value, element, kept)}, kept);
    lowering_.builder_.CreateBr(nextBlock);
    lowering_.functionFrame().switchTo(dropBlock);
    lowering_.refCounts().releaseValue(value, element);
    break;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
    lowering_.builder_.CreateStore(result, accSlot);
    break;
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    test(result);
    lowering_.refCounts().releaseValue(value, element);
    lowering_.builder_.CreateBr(nextBlock);
    lowering_.functionFrame().switchTo(foundBlock);
    lowering_.builder_.CreateStore(
        lowering_.optionalLowerer().makeOptional(value, lowering_.builder_.getTrue(), call->type),
        resultSlot);
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(lowering_.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    test(result);
    lowering_.builder_.CreateBr(nextBlock);
    lowering_.functionFrame().switchTo(foundBlock);
    lowering_.builder_.CreateStore(i, resultSlot);
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(lowering_.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrSome:
  case Builtin::ArrEvery: {
    bool some = method == Builtin::ArrSome;
    test(some ? result : lowering_.builder_.CreateNot(result));
    lowering_.builder_.CreateBr(nextBlock);
    lowering_.functionFrame().switchTo(foundBlock);
    lowering_.builder_.CreateStore(lowering_.builder_.getInt1(some), resultSlot);
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(lowering_.functionFrame().newBlock("dead"));
    break;
  }
  default:
    llvm_unreachable("not a method with a callback");
  }
  lowering_.functionFrame().branchTo(nextBlock);

  lowering_.functionFrame().switchTo(nextBlock);
  lowering_.builder_.CreateStore(lowering_.builder_.CreateAdd(i, step), indexSlot);
  lowering_.builder_.CreateBr(headBlock);

  lowering_.functionFrame().switchTo(endBlock);
  lowering_.refCounts().releaseValue(callback, callbackType);
  lowering_.refCounts().releaseValue(array, arrayType);
  if (out) {
    return out;
  }
  if (reduce) {
    return reduceResult(call, length, accSlot, accType);
  }
  if (resultSlot) {
    return lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(call->type), resultSlot);
  }

  return nullptr;
}

llvm::Value *
CallbackMethodLowerer::firstOfReduce(CallExpr *call, llvm::Value *length, llvm::Value *start,
                                     llvm::Value *step, llvm::Value *accSlot, const Type *accType,
                                     llvm::function_ref<llvm::Value *(llvm::Value *)> readElement) {
  const Type *element = accType;
  auto takeFirst = [&] {
    llvm::Value *first = readElement(start);
    lowering_.refCounts().retainValue(first, element);
    lowering_.builder_.CreateStore(first, accSlot);
    return lowering_.builder_.CreateAdd(start, step);
  };
  if (!call->mayBeOutOfRange) {
    return takeFirst();
  }
  llvm::BasicBlock *emptyBlock = lowering_.functionFrame().newBlock("reduce.empty");
  llvm::BasicBlock *firstBlock = lowering_.functionFrame().newBlock("reduce.first");
  llvm::BasicBlock *joinBlock = lowering_.functionFrame().newBlock("reduce.join");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpEQ(length, lowering_.builder_.getInt64(0)), emptyBlock,
      firstBlock, llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(emptyBlock);
  llvm::Type *accTy = lowering_.moduleBuilder().lower(accType);
  lowering_.builder_.CreateStore(accType == call->type
                                     ? lowering_.optionalLowerer().undefinedValue(accType)
                                     : llvm::Constant::getNullValue(accTy),
                                 accSlot);
  lowering_.builder_.CreateBr(joinBlock);
  lowering_.functionFrame().switchTo(firstBlock);
  llvm::Value *next = takeFirst();
  llvm::BasicBlock *firstEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(joinBlock);
  lowering_.functionFrame().switchTo(joinBlock);
  llvm::PHINode *joined = lowering_.builder_.CreatePHI(start->getType(), 2, "start");
  joined->addIncoming(start, emptyBlock);
  joined->addIncoming(next, firstEnd);

  return joined;
}

llvm::Value *CallbackMethodLowerer::reduceResult(CallExpr *call, llvm::Value *length,
                                                 llvm::Value *accSlot, const Type *accType) {
  llvm::Value *acc =
      lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(accType), accSlot);
  if (accType == call->type) {
    return acc;
  }

  return lowering_.optionalLowerer().makeOptional(
      acc, lowering_.builder_.CreateICmpNE(length, lowering_.builder_.getInt64(0)), call->type);
}

llvm::Function *CallbackMethodLowerer::sortComparator(CallExpr *call) {
  const Type *receiver = cast<MemberExpr>(call->callee)->object->type;
  const Type *element = receiver->isTypedArray() ? receiver->elementType() : receiver->inner();
  Expr *callbackExpr = call->args[0];
  const Type *callbackType = callbackExpr->type;
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto *type = llvm::FunctionType::get(lowering_.builder_.getInt32Ty(), {ptr, ptr, ptr}, false);
  auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.compare",
                                    lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = lowering_.function_;
  lowering_.function_ = fn;
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
  llvm::Value *callback = lowering_.builder_.CreateLoad(lowering_.fnTy_, fn->getArg(0), "compare");
  size_t passes = std::min<size_t>(callbackType->params().size(), 2);
  std::vector<llvm::Value *> args;
  for (size_t k = 0; k < passes; ++k) {
    llvm::Value *value = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element),
                                                       fn->getArg(unsigned(k + 1)));
    lowering_.refCounts().retainValue(value, element);
    args.push_back(value);
  }
  llvm::Value *result;
  if (auto *fe = dyn_cast<FunctionExpr>(callbackExpr)) {
    std::vector<llvm::Value *> all{lowering_.builder_.CreateExtractValue(callback, 1)};
    all.insert(all.end(), args.begin(), args.end());
    for (size_t k = passes; k < callbackType->params().size(); ++k) {
      all.push_back(lowering_.optionalLowerer().undefinedValue(callbackType->params()[k]));
    }
    result = lowering_.builder_.CreateCall(lowering_.closureLowerer().closureCode(fe->fn), all);
  } else {
    result = lowering_.closureLowerer().callFunctionValue(callback, callbackType, args);
  }
  const Type *resultType = callbackType->ret();
  if (resultType->isDecimal()) {
    llvm::Value *sign = lowering_.decimalLowerer().decimalSign(result);
    lowering_.refCounts().releaseValue(result, resultType);
    lowering_.builder_.CreateRet(sign);
    lowering_.function_ = outer;
    lowering_.builder_.restoreIP(point);
    return fn;
  }
  llvm::Value *positive, *negative;
  if (resultType->isFloat()) {
    llvm::Value *zero = llvm::ConstantFP::get(lowering_.builder_.getDoubleTy(), 0.0);
    positive = lowering_.builder_.CreateFCmpOGT(result, zero);
    negative = lowering_.builder_.CreateFCmpOLT(result, zero);
  } else {
    llvm::Value *zero = llvm::ConstantInt::get(result->getType(), 0);
    positive = resultType->isUnsigned() ? lowering_.builder_.CreateICmpNE(result, zero)
                                        : lowering_.builder_.CreateICmpSGT(result, zero);
    negative = resultType->isUnsigned() ? lowering_.builder_.getFalse()
                                        : lowering_.builder_.CreateICmpSLT(result, zero);
  }
  lowering_.builder_.CreateRet(lowering_.builder_.CreateSub(
      lowering_.builder_.CreateZExt(positive, lowering_.builder_.getInt32Ty()),
      lowering_.builder_.CreateZExt(negative, lowering_.builder_.getInt32Ty())));
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return fn;
}

}
