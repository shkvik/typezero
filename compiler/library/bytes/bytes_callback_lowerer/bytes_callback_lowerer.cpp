#include "compiler/library/bytes/bytes_callback_lowerer/bytes_callback_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/callback_method_lowerer/callback_method_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

BytesCallbackLowerer::BytesCallbackLowerer(CodeGen &codegen) : codegen_(codegen) {}

// forEach, map, filter, reduce, reduceRight, find, findLast, findIndex,
// findLastIndex, some, every and sort(compare) of a typed array. Its length
// never changes, and its elements are numbers: nothing to count.
llvm::Value *BytesCallbackLowerer::emitBytesCallbackMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->elementType();
  const Type *callbackType = call->args[0]->type;
  llvm::Type *i64 = codegen_.builder_.getInt64Ty(), *ptr = codegen_.builder_.getPtrTy();
  Builtin method = call->builtin;
  bool throws = codegen_.typeThrows(callbackType);
  bool reduce = method == Builtin::ArrReduce || method == Builtin::ArrReduceRight;
  bool emptyThrows = reduce && call->args.size() < 2;
  llvm::Value *array = codegen_.exprLowerer().emitExpr(member->object);
  CodeGen::Held heldArray = codegen_.throwLowerer().hold(array, arrayType,
                        throws || emptyThrows ||
                            llvm::any_of(call->args, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));
  llvm::Value *callback = codegen_.exprLowerer().emitExpr(call->args[0]);
  CodeGen::Held heldCallback =
      codegen_.throwLowerer().hold(callback, callbackType,
           throws || emptyThrows || (call->args.size() > 1 && codegen_.mayThrow().exprThrows(call->args[1])));
  llvm::Value *init = call->args.size() > 1 ? codegen_.exprLowerer().emitExpr(call->args[1]) : nullptr;

  if (method == Builtin::ArrSortWith) {
    // toSorted(compare) sorts a copy.
    bool copy = member->name == "toSorted";
    llvm::Value *target = array;
    CodeGen::Held heldCopy{nullptr, nullptr, nullptr};
    if (copy) {
      target = codegen_.builder_.CreateCall(
          codegen_.processLowerer().processFunction("tz_bytes_slice", ptr, {ptr, i64, i64, codegen_.builder_.getInt32Ty()}),
          {array, codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(0), codegen_.builder_.getInt32(0)}, "sorted");
      heldCopy = codegen_.throwLowerer().hold(target, call->type, throws);
    }
    llvm::Value *context = codegen_.functionFrame().spill(callback, "compare");
    codegen_.builder_.CreateCall(
        codegen_.processLowerer().processFunction("tz_bytes_sort_with", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {target, codegen_.callbackMethodLowerer().sortComparator(call), context});
    codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldCallback), callbackType);
    codegen_.emitThrowCheckIf(throws);
    if (!copy) {
      return codegen_.throwLowerer().take(heldArray); // the same array, with this reference
    }
    codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldArray), arrayType);
    return codegen_.throwLowerer().take(heldCopy);
  }

  bool backward = method == Builtin::ArrReduceRight || method == Builtin::ArrFindLast ||
                  method == Builtin::ArrFindLastIndex;
  size_t passes = std::min(callbackType->params().size(), size_t(reduce ? 4 : 3));
  size_t valueArgs = reduce ? 2 : 1;
  llvm::Value *length = codegen_.bytesLowerer().bytesLength(array);
  llvm::Value *start =
      backward ? codegen_.builder_.CreateSub(length, codegen_.builder_.getInt64(1)) : codegen_.builder_.getInt64(0);
  llvm::Value *step = codegen_.builder_.getInt64(backward ? -1 : 1);

  llvm::Value *accSlot = nullptr;
  if (reduce) {
    accSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(call->type), "acc");
    if (init) {
      codegen_.builder_.CreateStore(init, accSlot);
    } else {
      llvm::BasicBlock *emptyBlock = codegen_.functionFrame().newBlock("reduce.empty");
      llvm::BasicBlock *firstBlock = codegen_.functionFrame().newBlock("reduce.first");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(length, codegen_.builder_.getInt64(0)), emptyBlock,
                            firstBlock, llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
      codegen_.functionFrame().switchTo(emptyBlock);
      codegen_.throwLowerer().emitThrowNew(1, "Reduce of empty array with no initial value");
      codegen_.functionFrame().switchTo(firstBlock);
      codegen_.builder_.CreateStore(codegen_.bytesLowerer().loadElement(codegen_.bytesLowerer().bytesData(array), arrayType, start), accSlot);
      start = codegen_.builder_.CreateAdd(start, step);
    }
  }
  // map: a new typed array of the same kind, of the same length; filter: an
  // array of the elements kept, which becomes one at the end.
  llvm::Value *out = nullptr;
  int32_t kind = int32_t(arrayType->bytes());
  if (method == Builtin::ArrMap) {
    out = codegen_.builder_.CreateCall(
        codegen_.processLowerer().processFunction("tz_bytes_new_unset", ptr, {codegen_.builder_.getInt32Ty(), i64}),
        {codegen_.builder_.getInt32(uint32_t(kind)), length}, "mapped");
  } else if (method == Builtin::ArrFilter) {
    out = codegen_.builder_.CreateCall(
        codegen_.rt_.arrNew, {codegen_.typeDescriptors().typeDescriptor(element), codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(0)}, "kept");
  }
  CodeGen::Held heldOut = codegen_.throwLowerer().hold(out, method == Builtin::ArrMap ? call->type : Type::array(element), throws);
  llvm::Value *resultSlot = nullptr;
  if (!reduce && !out && method != Builtin::ArrForEach) {
    resultSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(call->type), "result");
    llvm::Value *none = method == Builtin::ArrSome    ? codegen_.builder_.getFalse()
                        : method == Builtin::ArrEvery ? codegen_.builder_.getTrue()
                        : call->type->isInt()         ? codegen_.builder_.getInt32(~0u)
                                                      : codegen_.optionalLowerer().undefinedValue(call->type);
    codegen_.builder_.CreateStore(none, resultSlot);
  }

  llvm::Value *indexSlot = codegen_.functionFrame().createStackSlot(i64, "index");
  codegen_.builder_.CreateStore(start, indexSlot);
  llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("each");
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("each.body");
  llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("each.next");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("each.end");
  codegen_.functionFrame().emitBlock(headBlock);
  llvm::Value *i = codegen_.builder_.CreateLoad(i64, indexSlot, "i");
  // Going back, the unsigned comparison stops below 0 too.
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(i, length), bodyBlock, endBlock);

  codegen_.functionFrame().switchTo(bodyBlock);
  llvm::Value *value = codegen_.bytesLowerer().loadElement(codegen_.bytesLowerer().bytesData(array), arrayType, i);
  std::vector<llvm::Value *> args;
  if (reduce) {
    args.push_back(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(call->type), accSlot));
  }
  args.push_back(value);
  args.push_back(codegen_.builder_.CreateTrunc(i, codegen_.builder_.getInt32Ty()));
  args.push_back(array);
  if (reduce && passes == 0) {
    codegen_.refCounts().releaseValue(args[0], call->type);
  }
  args.resize(passes);
  if (passes == valueArgs + 2) {
    codegen_.refCounts().retainValue(array, arrayType); // the callback owns its arguments
  }
  llvm::Value *result = codegen_.closureLowerer().callFunctionValue(callback, callbackType, args);
  codegen_.emitThrowCheckIf(throws);
  const Type *resultType = callbackType->ret();

  llvm::BasicBlock *foundBlock = nullptr;
  auto test = [&](llvm::Value *decides) {
    foundBlock = codegen_.functionFrame().newBlock("each.found");
    llvm::BasicBlock *otherBlock = codegen_.functionFrame().newBlock("each.other");
    codegen_.builder_.CreateCondBr(decides, foundBlock, otherBlock);
    codegen_.functionFrame().switchTo(otherBlock);
  };
  switch (method) {
  case Builtin::ArrForEach:
    if (result) {
      codegen_.refCounts().releaseValue(result, resultType);
    }
    break;
  case Builtin::ArrMap:
    codegen_.bytesLowerer().storeElement(codegen_.bytesLowerer().bytesData(out), arrayType, i, result);
    break;
  case Builtin::ArrFilter: {
    llvm::BasicBlock *keepBlock = codegen_.functionFrame().newBlock("filter.keep");
    codegen_.builder_.CreateCondBr(result, keepBlock, nextBlock);
    codegen_.functionFrame().switchTo(keepBlock);
    codegen_.arrayLowerer().emitPush(out, {value}, element);
    break;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
    codegen_.builder_.CreateStore(result, accSlot);
    break;
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    test(result);
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(foundBlock);
    codegen_.builder_.CreateStore(codegen_.optionalLowerer().makeOptional(value, codegen_.builder_.getTrue(), call->type), resultSlot);
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(codegen_.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    test(result);
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(foundBlock);
    codegen_.builder_.CreateStore(codegen_.builder_.CreateTrunc(i, codegen_.builder_.getInt32Ty()), resultSlot);
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(codegen_.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrSome:
  case Builtin::ArrEvery: {
    bool some = method == Builtin::ArrSome;
    test(some ? result : codegen_.builder_.CreateNot(result));
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(foundBlock);
    codegen_.builder_.CreateStore(codegen_.builder_.getInt1(some), resultSlot);
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(codegen_.functionFrame().newBlock("dead"));
    break;
  }
  default:
    llvm_unreachable("not a method with a callback");
  }
  codegen_.functionFrame().branchTo(nextBlock);

  codegen_.functionFrame().switchTo(nextBlock);
  codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(i, step), indexSlot);
  codegen_.builder_.CreateBr(headBlock);

  codegen_.functionFrame().switchTo(endBlock);
  codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldCallback), callbackType);
  codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldArray), arrayType);
  if (method == Builtin::ArrMap) {
    return codegen_.throwLowerer().take(heldOut);
  }
  if (method == Builtin::ArrFilter) {
    llvm::Value *kept = codegen_.throwLowerer().take(heldOut);
    llvm::Value *made = codegen_.builder_.CreateCall(
        codegen_.processLowerer().processFunction("tz_bytes_from_array", ptr, {codegen_.builder_.getInt32Ty(), ptr}),
        {codegen_.builder_.getInt32(uint32_t(kind)), kept}, "filtered");
    codegen_.refCounts().releaseValue(kept, Type::array(element));
    return made;
  }
  if (reduce) {
    return codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(call->type), accSlot);
  }
  if (resultSlot) {
    return codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(call->type), resultSlot);
  }

  return nullptr;
}

} // namespace tz
