#include "compiler/lower/values/callback_method_lowerer/callback_method_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/library/bytes/bytes_callback_lowerer/bytes_callback_lowerer.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

enum ArrayField : unsigned { kRefCount, kElementType, kData, kLength, kCapacity, kAlloc };

}

CallbackMethodLowerer::CallbackMethodLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *CallbackMethodLowerer::emitCallbackMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  if (member->object->type->isTypedArray()) {
    return this->lowering.bytesCallbackLowerer().emitBytesCallbackMethod(call);
  }
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  const Type *callbackType = call->args[0]->type;
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  Builtin method = call->builtin();

  llvm::Value *array = this->lowering.exprLowerer().emitExpr(member->object);
  llvm::Value *callback = this->lowering.exprLowerer().emitExpr(call->args[0]);
  llvm::Value *init =
      call->args.size() > 1 ? this->lowering.exprLowerer().emitExpr(call->args[1]) : nullptr;

  if (method == Builtin::ArrSortWith) {
    llvm::Value *context = this->lowering.functionFrame().spill(callback, "compare");
    this->lowering.builder.CreateCall(this->lowering.rt_.arrSortWith,
                                      {array, this->sortComparator(call), context});
    this->lowering.refCounts().releaseValue(callback, callbackType);
    return array;
  }

  bool reduce = method == Builtin::ArrReduce || method == Builtin::ArrReduceRight;
  bool backward = method == Builtin::ArrReduceRight || method == Builtin::ArrFindLast ||
                  method == Builtin::ArrFindLastIndex;
  size_t passes = std::min(callbackType->params().size(), size_t(reduce ? 4 : 3));
  size_t valueArgs = reduce ? 2 : 1;

  llvm::Value *length = this->lowering.arrayLowerer().arrayLength(array);
  llvm::Value *start =
      backward ? this->lowering.builder.CreateSub(length, this->lowering.builder.getInt64(1))
               : this->lowering.builder.getInt64(0);
  llvm::Value *step = this->lowering.builder.getInt64(backward ? -1 : 1);
  auto readElement = [&](llvm::Value *at) {
    return this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(element),
        this->lowering.arrayLowerer().elementAddress(this->lowering.arrayLowerer().arrayData(array),
                                                     element, at),
        "element");
  };

  llvm::Value *accSlot = nullptr;
  const Type *accType = reduce && !init ? element : call->type;
  if (reduce) {
    accSlot = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(accType), "acc");
    if (init) {
      this->lowering.builder.CreateStore(init, accSlot);
    } else {
      start = this->firstOfReduce(call, length, start, step, accSlot, accType, readElement);
    }
  }
  llvm::Value *out = nullptr;
  if (method == Builtin::ArrMap || method == Builtin::ArrFilter || method == Builtin::ArrFlatMap) {
    out = this->lowering.groupLowerer().newArray(
        call->type, this->lowering.builder.getInt64(0),
        method == Builtin::ArrMap ? length : this->lowering.builder.getInt64(0),
        this->lowering.makesMember(call, call->type), "result");
  }
  if (out) {
    this->lowering.groupLowerer().noteUnstored(out, call);
  }
  llvm::Value *resultSlot = nullptr;
  if (!reduce && !out && method != Builtin::ArrForEach) {
    resultSlot = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(call->type), "result");
    llvm::Value *none = method == Builtin::ArrSome    ? this->lowering.builder.getFalse()
                        : method == Builtin::ArrEvery ? this->lowering.builder.getTrue()
                        : call->type->isInt()
                            ? this->lowering.builder.getInt64(~uint64_t(0))
                            : this->lowering.optionalLowerer().undefinedValue(call->type);
    this->lowering.builder.CreateStore(none, resultSlot);
  }

  llvm::Value *indexSlot = this->lowering.functionFrame().createStackSlot(i64, "index");
  this->lowering.builder.CreateStore(start, indexSlot);
  llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("each");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("each.body");
  llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("each.next");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("each.end");
  this->lowering.functionFrame().emitBlock(headBlock);
  llvm::Value *i = this->lowering.builder.CreateLoad(i64, indexSlot, "i");
  llvm::Value *inRange =
      this->lowering.builder.CreateICmpULT(i, this->lowering.arrayLowerer().arrayLength(array));
  if (!backward) {
    inRange =
        this->lowering.builder.CreateAnd(inRange, this->lowering.builder.CreateICmpULT(i, length));
  }
  this->lowering.builder.CreateCondBr(inRange, bodyBlock, endBlock);

  this->lowering.functionFrame().switchTo(bodyBlock);
  llvm::Value *value = readElement(i);
  bool keeps =
      method == Builtin::ArrFilter || method == Builtin::ArrFind || method == Builtin::ArrFindLast;
  if (passes >= valueArgs) {
    this->lowering.refCounts().retainValue(value, element);
  }
  if (keeps) {
    this->lowering.refCounts().retainValue(value, element);
  }
  std::vector<llvm::Value *> args;
  if (reduce) {
    args.push_back(
        this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(accType), accSlot));
  }
  args.push_back(value);
  args.push_back(i);
  args.push_back(array);
  if (reduce && passes == 0) {
    this->lowering.refCounts().releaseValue(args[0], accType);
  }
  args.resize(passes);
  if (passes == valueArgs + 2) {
    this->lowering.refCounts().retainValue(array, arrayType);
  }
  llvm::Value *result =
      this->lowering.closureLowerer().callFunctionValue(callback, callbackType, args);
  const Type *resultType = callbackType->ret();

  llvm::BasicBlock *foundBlock = nullptr;
  auto test = [&](llvm::Value *decides) {
    foundBlock = this->lowering.functionFrame().newBlock("each.found");
    llvm::BasicBlock *otherBlock = this->lowering.functionFrame().newBlock("each.other");
    this->lowering.builder.CreateCondBr(decides, foundBlock, otherBlock);
    this->lowering.functionFrame().switchTo(otherBlock);
  };
  switch (method) {
  case Builtin::ArrForEach:
    if (result) {
      this->lowering.refCounts().releaseValue(result, resultType);
    }
    break;
  case Builtin::ArrMap:
    this->lowering.builder.CreateStore(
        result, this->lowering.arrayLowerer().elementAddress(
                    this->lowering.arrayLowerer().arrayData(out), resultType, i));
    this->lowering.builder.CreateStore(
        this->lowering.builder.CreateTrunc(
            this->lowering.builder.CreateAdd(i, this->lowering.builder.getInt64(1)),
            this->lowering.builder.getInt32Ty()),
        this->lowering.arrayLowerer().arrayField(out, kLength));
    break;
  case Builtin::ArrFlatMap:
    if (resultType->isArray()) {
      this->lowering.builder.CreateCall(this->lowering.rt_.arrAppend, {out, result});
      this->lowering.refCounts().releaseValue(result, resultType);
    } else {
      this->lowering.arrayLowerer().emitPush(out, {result}, resultType);
    }
    break;
  case Builtin::ArrFilter: {
    llvm::BasicBlock *keepBlock = this->lowering.functionFrame().newBlock("filter.keep");
    llvm::BasicBlock *dropBlock = this->lowering.functionFrame().newBlock("filter.drop");
    this->lowering.builder.CreateCondBr(result, keepBlock, dropBlock);
    this->lowering.functionFrame().switchTo(keepBlock);
    const Type *kept = call->type->inner();
    this->lowering.arrayLowerer().emitPush(
        out, {this->lowering.exprLowerer().emitConversion(value, element, kept)}, kept);
    this->lowering.builder.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(dropBlock);
    this->lowering.refCounts().releaseValue(value, element);
    break;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
    this->lowering.builder.CreateStore(result, accSlot);
    break;
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    test(result);
    this->lowering.refCounts().releaseValue(value, element);
    this->lowering.builder.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(foundBlock);
    this->lowering.builder.CreateStore(this->lowering.optionalLowerer().makeOptional(
                                           value, this->lowering.builder.getTrue(), call->type),
                                       resultSlot);
    this->lowering.builder.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(this->lowering.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    test(result);
    this->lowering.builder.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(foundBlock);
    this->lowering.builder.CreateStore(i, resultSlot);
    this->lowering.builder.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(this->lowering.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrSome:
  case Builtin::ArrEvery: {
    bool some = method == Builtin::ArrSome;
    test(some ? result : this->lowering.builder.CreateNot(result));
    this->lowering.builder.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(foundBlock);
    this->lowering.builder.CreateStore(this->lowering.builder.getInt1(some), resultSlot);
    this->lowering.builder.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(this->lowering.functionFrame().newBlock("dead"));
    break;
  }
  default:
    llvm_unreachable("not a method with a callback");
  }
  this->lowering.functionFrame().branchTo(nextBlock);

  this->lowering.functionFrame().switchTo(nextBlock);
  this->lowering.builder.CreateStore(this->lowering.builder.CreateAdd(i, step), indexSlot);
  this->lowering.builder.CreateBr(headBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  this->lowering.refCounts().releaseValue(callback, callbackType);
  this->lowering.refCounts().releaseValue(array, arrayType);
  if (out) {
    return out;
  }
  if (reduce) {
    return this->reduceResult(call, length, accSlot, accType);
  }
  if (resultSlot) {
    return this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(call->type),
                                             resultSlot);
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
    this->lowering.refCounts().retainValue(first, element);
    this->lowering.builder.CreateStore(first, accSlot);
    return this->lowering.builder.CreateAdd(start, step);
  };
  if (!call->mayBeOutOfRange) {
    return takeFirst();
  }
  llvm::BasicBlock *emptyBlock = this->lowering.functionFrame().newBlock("reduce.empty");
  llvm::BasicBlock *firstBlock = this->lowering.functionFrame().newBlock("reduce.first");
  llvm::BasicBlock *joinBlock = this->lowering.functionFrame().newBlock("reduce.join");
  this->lowering.builder.CreateCondBr(
      this->lowering.builder.CreateICmpEQ(length, this->lowering.builder.getInt64(0)), emptyBlock,
      firstBlock, llvm::MDBuilder(this->lowering.ctx).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(emptyBlock);
  llvm::Type *accTy = this->lowering.moduleBuilder().lower(accType);
  this->lowering.builder.CreateStore(accType == call->type
                                         ? this->lowering.optionalLowerer().undefinedValue(accType)
                                         : llvm::Constant::getNullValue(accTy),
                                     accSlot);
  this->lowering.builder.CreateBr(joinBlock);
  this->lowering.functionFrame().switchTo(firstBlock);
  llvm::Value *next = takeFirst();
  llvm::BasicBlock *firstEnd = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(joinBlock);
  this->lowering.functionFrame().switchTo(joinBlock);
  llvm::PHINode *joined = this->lowering.builder.CreatePHI(start->getType(), 2, "start");
  joined->addIncoming(start, emptyBlock);
  joined->addIncoming(next, firstEnd);

  return joined;
}

llvm::Value *CallbackMethodLowerer::reduceResult(CallExpr *call, llvm::Value *length,
                                                 llvm::Value *accSlot, const Type *accType) {
  llvm::Value *acc =
      this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(accType), accSlot);
  if (accType == call->type) {
    return acc;
  }

  return this->lowering.optionalLowerer().makeOptional(
      acc, this->lowering.builder.CreateICmpNE(length, this->lowering.builder.getInt64(0)),
      call->type);
}

llvm::Function *CallbackMethodLowerer::sortComparator(CallExpr *call) {
  const Type *receiver = cast<MemberExpr>(call->callee)->object->type;
  const Type *element = receiver->isTypedArray() ? receiver->elementType() : receiver->inner();
  Expr *callbackExpr = call->args[0];
  const Type *callbackType = callbackExpr->type;
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  auto *type = llvm::FunctionType::get(this->lowering.builder.getInt32Ty(), {ptr, ptr, ptr}, false);
  auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.compare",
                                    this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
  llvm::Function *outer = this->lowering.function;
  this->lowering.function = fn;
  this->lowering.builder.SetInsertPoint(llvm::BasicBlock::Create(this->lowering.ctx, "entry", fn));
  llvm::Value *callback =
      this->lowering.builder.CreateLoad(this->lowering.fnTy, fn->getArg(0), "compare");
  size_t passes = std::min<size_t>(callbackType->params().size(), 2);
  std::vector<llvm::Value *> args;
  for (size_t k = 0; k < passes; ++k) {
    llvm::Value *value = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(element), fn->getArg(unsigned(k + 1)));
    this->lowering.refCounts().retainValue(value, element);
    args.push_back(value);
  }
  llvm::Value *result;
  auto *fe = dyn_cast<FunctionExpr>(callbackExpr);
  if (fe) {
    std::vector<llvm::Value *> all{this->lowering.builder.CreateExtractValue(callback, 1)};
    all.insert(all.end(), args.begin(), args.end());
    for (size_t k = passes; k < callbackType->params().size(); ++k) {
      all.push_back(this->lowering.optionalLowerer().undefinedValue(callbackType->params()[k]));
    }
    result =
        this->lowering.builder.CreateCall(this->lowering.closureLowerer().closureCode(fe->fn), all);
  } else {
    result = this->lowering.closureLowerer().callFunctionValue(callback, callbackType, args);
  }
  const Type *resultType = callbackType->ret();
  if (resultType->isDecimal()) {
    llvm::Value *sign = this->lowering.decimalLowerer().decimalSign(result);
    this->lowering.refCounts().releaseValue(result, resultType);
    this->lowering.builder.CreateRet(sign);
    this->lowering.function = outer;
    this->lowering.builder.restoreIP(point);
    return fn;
  }
  llvm::Value *positive, *negative;
  if (resultType->isFloat()) {
    llvm::Value *zero = llvm::ConstantFP::get(this->lowering.builder.getDoubleTy(), 0.0);
    positive = this->lowering.builder.CreateFCmpOGT(result, zero);
    negative = this->lowering.builder.CreateFCmpOLT(result, zero);
  } else {
    llvm::Value *zero = llvm::ConstantInt::get(result->getType(), 0);
    positive = resultType->isUnsigned() ? this->lowering.builder.CreateICmpNE(result, zero)
                                        : this->lowering.builder.CreateICmpSGT(result, zero);
    negative = resultType->isUnsigned() ? this->lowering.builder.getFalse()
                                        : this->lowering.builder.CreateICmpSLT(result, zero);
  }
  this->lowering.builder.CreateRet(this->lowering.builder.CreateSub(
      this->lowering.builder.CreateZExt(positive, this->lowering.builder.getInt32Ty()),
      this->lowering.builder.CreateZExt(negative, this->lowering.builder.getInt32Ty())));
  this->lowering.function = outer;
  this->lowering.builder.restoreIP(point);

  return fn;
}

}
