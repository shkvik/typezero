#include "compiler/library/bytes/bytes_callback_lowerer/bytes_callback_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/callback_method_lowerer/callback_method_lowerer.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include <numeric>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

BytesCallbackLowerer::BytesCallbackLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *BytesCallbackLowerer::emitBytesCallbackMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->elementType();
  const Type *callbackType = call->args[0]->type;
  llvm::Type *i64 = lowering_.builder_.getInt64Ty(), *ptr = lowering_.builder_.getPtrTy();
  Builtin method = call->builtin;
  bool throws = lowering_.typeThrows(callbackType);
  bool reduce = method == Builtin::ArrReduce || method == Builtin::ArrReduceRight;
  llvm::Value *array = lowering_.exprLowerer().emitExpr(member->object);
  Lowering::Held heldArray = lowering_.throwLowerer().hold(
      array, arrayType, throws || llvm::any_of(call->args, [&](Expr *arg) {
                          return lowering_.mayThrow().exprThrows(arg);
                        }));
  llvm::Value *callback = lowering_.exprLowerer().emitExpr(call->args[0]);
  Lowering::Held heldCallback = lowering_.throwLowerer().hold(
      callback, callbackType,
      throws || (call->args.size() > 1 && lowering_.mayThrow().exprThrows(call->args[1])));
  llvm::Value *init =
      call->args.size() > 1 ? lowering_.exprLowerer().emitExpr(call->args[1]) : nullptr;

  if (method == Builtin::ArrSortWith) {
    bool copy = member->name == "toSorted";
    llvm::Value *target = array;
    Lowering::Held heldCopy{nullptr, nullptr, nullptr};
    if (copy) {
      target = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction(
              "tz_bytes_slice", ptr, {ptr, i64, i64, lowering_.builder_.getInt32Ty()}),
          {array, lowering_.builder_.getInt64(0), lowering_.builder_.getInt64(0),
           lowering_.builder_.getInt32(0)},
          "sorted");
      heldCopy = lowering_.throwLowerer().hold(target, call->type, throws);
    }
    llvm::Value *context = lowering_.functionFrame().spill(callback, "compare");
    lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_bytes_sort_with",
                                                   lowering_.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {target, lowering_.callbackMethodLowerer().sortComparator(call), context});
    lowering_.refCounts().releaseValue(lowering_.throwLowerer().take(heldCallback), callbackType);
    lowering_.emitThrowCheckIf(throws);
    if (!copy) {
      return lowering_.throwLowerer().take(heldArray);
    }
    lowering_.refCounts().releaseValue(lowering_.throwLowerer().take(heldArray), arrayType);
    return lowering_.throwLowerer().take(heldCopy);
  }

  bool backward = method == Builtin::ArrReduceRight || method == Builtin::ArrFindLast ||
                  method == Builtin::ArrFindLastIndex;
  size_t passes = std::min(callbackType->params().size(), size_t(reduce ? 4 : 3));
  size_t valueArgs = reduce ? 2 : 1;
  llvm::Value *length = lowering_.bytesLowerer().bytesLength(array);
  llvm::Value *start = backward
                           ? lowering_.builder_.CreateSub(length, lowering_.builder_.getInt64(1))
                           : lowering_.builder_.getInt64(0);
  llvm::Value *step = lowering_.builder_.getInt64(backward ? -1 : 1);

  llvm::Value *accSlot = nullptr;
  const Type *accType = reduce && !init ? element : call->type;
  if (reduce) {
    accSlot =
        lowering_.functionFrame().createStackSlot(lowering_.moduleBuilder().lower(accType), "acc");
    if (init) {
      lowering_.builder_.CreateStore(init, accSlot);
    } else {
      start = lowering_.callbackMethodLowerer().firstOfReduce(
          call, length, start, step, accSlot, accType, [&](llvm::Value *at) {
            return lowering_.bytesLowerer().loadElement(lowering_.bytesLowerer().bytesData(array),
                                                        arrayType, at);
          });
    }
  }
  llvm::Value *out = nullptr;
  int32_t kind = int32_t(arrayType->bytes());
  if (method == Builtin::ArrMap) {
    out = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_bytes_new_unset", ptr,
                                                   {lowering_.builder_.getInt32Ty(), i64}),
        {lowering_.builder_.getInt32(uint32_t(kind)), length}, "mapped");
  } else if (method == Builtin::ArrFilter) {
    out = lowering_.builder_.CreateCall(lowering_.rt_.arrNew,
                                        {lowering_.typeDescriptors().typeDescriptor(element),
                                         lowering_.builder_.getInt64(0),
                                         lowering_.builder_.getInt64(0)},
                                        "kept");
  }
  Lowering::Held heldOut = lowering_.throwLowerer().hold(
      out, method == Builtin::ArrMap ? call->type : Type::array(element), throws);
  llvm::Value *resultSlot = nullptr;
  if (!reduce && !out && method != Builtin::ArrForEach) {
    resultSlot = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(call->type), "result");
    llvm::Value *none = method == Builtin::ArrSome    ? lowering_.builder_.getFalse()
                        : method == Builtin::ArrEvery ? lowering_.builder_.getTrue()
                        : call->type->isInt()
                            ? lowering_.builder_.getInt32(~0u)
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
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(i, length), bodyBlock, endBlock);

  lowering_.functionFrame().switchTo(bodyBlock);
  llvm::Value *value =
      lowering_.bytesLowerer().loadElement(lowering_.bytesLowerer().bytesData(array), arrayType, i);
  std::vector<llvm::Value *> args;
  if (reduce) {
    args.push_back(
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(accType), accSlot));
  }
  args.push_back(value);
  args.push_back(lowering_.builder_.CreateTrunc(i, lowering_.builder_.getInt32Ty()));
  args.push_back(array);
  if (reduce && passes == 0) {
    lowering_.refCounts().releaseValue(args[0], accType);
  }
  args.resize(passes);
  if (passes == valueArgs + 2) {
    lowering_.refCounts().retainValue(array, arrayType);
  }
  llvm::Value *result = lowering_.closureLowerer().callFunctionValue(callback, callbackType, args);
  lowering_.emitThrowCheckIf(throws);
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
    lowering_.bytesLowerer().storeElement(lowering_.bytesLowerer().bytesData(out), arrayType, i,
                                          result);
    break;
  case Builtin::ArrFilter: {
    llvm::BasicBlock *keepBlock = lowering_.functionFrame().newBlock("filter.keep");
    lowering_.builder_.CreateCondBr(result, keepBlock, nextBlock);
    lowering_.functionFrame().switchTo(keepBlock);
    lowering_.arrayLowerer().emitPush(out, {value}, element);
    break;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
    lowering_.builder_.CreateStore(result, accSlot);
    break;
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    test(result);
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
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateTrunc(i, lowering_.builder_.getInt32Ty()), resultSlot);
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
  lowering_.refCounts().releaseValue(lowering_.throwLowerer().take(heldCallback), callbackType);
  lowering_.refCounts().releaseValue(lowering_.throwLowerer().take(heldArray), arrayType);
  if (method == Builtin::ArrMap) {
    return lowering_.throwLowerer().take(heldOut);
  }
  if (method == Builtin::ArrFilter) {
    llvm::Value *kept = lowering_.throwLowerer().take(heldOut);
    llvm::Value *made = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_bytes_from_array", ptr,
                                                   {lowering_.builder_.getInt32Ty(), ptr}),
        {lowering_.builder_.getInt32(uint32_t(kind)), kept}, "filtered");
    lowering_.refCounts().releaseValue(kept, Type::array(element));
    return made;
  }
  if (reduce) {
    return lowering_.callbackMethodLowerer().reduceResult(call, length, accSlot, accType);
  }
  if (resultSlot) {
    return lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(call->type), resultSlot);
  }

  return nullptr;
}

}
