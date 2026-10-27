#include "compiler/lower/values/array_method_lowerer/array_method_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
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

ArrayMethodLowerer::ArrayMethodLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *ArrayMethodLowerer::emitArrayMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  Lowering::Operand array = lowering_.refCounts().emitOperand(
      member->object, llvm::all_of(call->args, AssignLowerer::cannotAssign));

  auto ownedArgs = [&](size_t from) {
    std::vector<llvm::Value *> values =
        lowering_.exprLowerer().emitExprs(llvm::ArrayRef<Expr *>(call->args).drop_front(from));

    return values;
  };
  auto position = [&](llvm::ArrayRef<llvm::Value *> values,
                      size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < values.size()) {
      return {values[i], lowering_.builder_.getInt32(1)};
    }

    return {lowering_.builder_.getInt64(0), lowering_.builder_.getInt32(0)};
  };
  auto returnArray = [&] {
    if (!array.owned) {
      lowering_.refCounts().retainValue(array.value, arrayType);
    }
    array.owned = false;

    return array.value;
  };

  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::ArrPush: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    std::vector<llvm::Value *> run;
    for (size_t i = 0; i < values.size(); ++i) {
      if (!call->spreads(i)) {
        run.push_back(values[i]);
        continue;
      }
      if (!run.empty()) {
        lowering_.arrayLowerer().emitPush(array.value, run, element);
      }
      run.clear();
      lowering_.builder_.CreateCall(lowering_.rt_.arrAppend, {array.value, values[i]});
      lowering_.refCounts().releaseValue(values[i], arrayType);
    }
    if (!run.empty()) {
      lowering_.arrayLowerer().emitPush(array.value, run, element);
    }
    result = lowering_.builder_.CreateTrunc(lowering_.arrayLowerer().arrayLength(array.value),
                                            lowering_.builder_.getInt32Ty());
    break;
  }
  case Builtin::ArrUnshift: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    llvm::Value *slots = lowering_.builder_.CreateCall(
        lowering_.rt_.arrUnshiftSlots, {array.value, lowering_.builder_.getInt64(values.size())});
    for (size_t i = 0; i < values.size(); ++i) {
      lowering_.builder_.CreateStore(
          values[i],
          lowering_.arrayLowerer().elementAddress(slots, element, lowering_.builder_.getInt64(i)));
      if (lowering_.memory().refMayBeMember(arrayType)) {
        lowering_.storeLowerer().groupStore(array.value, values[i], element);
      }
    }
    result = lowering_.builder_.CreateTrunc(lowering_.arrayLowerer().arrayLength(array.value),
                                            lowering_.builder_.getInt32Ty());
    break;
  }
  case Builtin::ArrPop: {
    llvm::Value *length = lowering_.arrayLowerer().arrayLength(array.value);
    auto pop = [&] {
      llvm::Value *last = lowering_.builder_.CreateSub(length, lowering_.builder_.getInt64(1));
      llvm::Value *value = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(element),
          lowering_.arrayLowerer().elementAddress(lowering_.arrayLowerer().arrayData(array.value),
                                                  element, last));
      lowering_.builder_.CreateStore(
          lowering_.builder_.CreateTrunc(last, lowering_.builder_.getInt32Ty()),
          lowering_.arrayLowerer().arrayField(array.value, kLength));
      if (lowering_.memory().refMayBeMember(arrayType)) {
        lowering_.storeLowerer().takeFrom(array.value, value, element);
      }

      return value;
    };
    result = call->mayBeOutOfRange
                 ? lowering_.arrayLowerer().emitCheckedRead(
                       lowering_.builder_.CreateICmpNE(length, lowering_.builder_.getInt64(0)),
                       call->type, pop)
                 : pop();
    break;
  }
  case Builtin::ArrShift: {
    llvm::Value *slot = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(element), "shifted");
    llvm::Value *has = lowering_.builder_.CreateCall(lowering_.rt_.arrShift, {array.value, slot});
    llvm::Value *value =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element), slot);
    if (lowering_.memory().refMayBeMember(arrayType)) {
      lowering_.storeLowerer().takeFrom(array.value, value, element);
    }
    result = call->mayBeOutOfRange
                 ? lowering_.optionalLowerer().makeOptional(
                       value, lowering_.builder_.CreateICmpNE(has, lowering_.builder_.getInt32(0)),
                       call->type)
                 : value;
    break;
  }
  case Builtin::ArrAt: {
    llvm::Value *length = lowering_.arrayLowerer().arrayLength(array.value);
    llvm::Value *at = lowering_.exprLowerer().emitExpr(call->args[0]);
    at = lowering_.builder_.CreateSelect(
        lowering_.builder_.CreateICmpSLT(at, lowering_.builder_.getInt64(0)),
        lowering_.builder_.CreateAdd(at, length), at);
    auto read = [&] {
      llvm::Value *value = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(element),
          lowering_.arrayLowerer().elementAddress(lowering_.arrayLowerer().arrayData(array.value),
                                                  element, at));
      lowering_.refCounts().retainValue(value, element);

      return value;
    };
    result = call->mayBeOutOfRange
                 ? lowering_.arrayLowerer().emitCheckedRead(
                       lowering_.builder_.CreateICmpULT(at, length), call->type, read)
                 : read();
    break;
  }
  case Builtin::ArrSlice: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    llvm::Value *start = values.empty() ? lowering_.builder_.getInt64(0) : values[0];
    auto [end, hasEnd] = position(values, 1);
    llvm::FunctionCallee slice = lowering_.rt_.arrSlice;
    if (lowering_.isCyclic(call->type)) {
      slice = lowering_.groupLowerer().groupFunction(
          lowering_.makesMember(call, call->type) ? "tz_arr_slice_group" : "tz_arr_slice_plain",
          lowering_.builder_.getPtrTy(),
          {lowering_.builder_.getPtrTy(), lowering_.builder_.getInt64Ty(),
           lowering_.builder_.getInt64Ty(), lowering_.builder_.getInt32Ty()});
    }
    result = lowering_.builder_.CreateCall(slice, {array.value, start, end, hasEnd}, "slice");
    lowering_.groupLowerer().noteUnstored(result, call);
    break;
  }
  case Builtin::ArrConcat: {
    std::vector<Lowering::Operand> parts;
    for (size_t i = 0; i < call->args.size(); ++i) {
      parts.push_back({lowering_.exprLowerer().emitExpr(call->args[i]), true, call->args[i]->type});
    }
    llvm::FunctionCallee copy = lowering_.rt_.arrCopy;
    if (lowering_.isCyclic(call->type)) {
      copy = lowering_.groupLowerer().groupFunction(
          lowering_.makesMember(call, call->type) ? "tz_arr_copy_group" : "tz_arr_copy_plain",
          lowering_.builder_.getPtrTy(), {lowering_.builder_.getPtrTy()});
    }
    result = lowering_.builder_.CreateCall(copy, {array.value}, "concat");
    lowering_.groupLowerer().noteUnstored(result, call);
    for (Lowering::Operand &part : parts) {
      if (part.type == arrayType) {
        lowering_.builder_.CreateCall(lowering_.rt_.arrAppend, {result, part.value});
        lowering_.refCounts().dropOperand(part);
      } else {
        lowering_.arrayLowerer().emitPush(result, {part.value}, element);
      }
    }
    break;
  }
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf:
  case Builtin::ArrIncludes: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    auto [from, hasFrom] = position(values, 1);
    int mode = call->builtin == Builtin::ArrIndexOf       ? 0
               : call->builtin == Builtin::ArrLastIndexOf ? 1
                                                          : 2;
    result = lowering_.builder_.CreateCall(lowering_.rt_.arrIndexOf,
                                           {array.value,
                                            lowering_.functionFrame().spill(values[0], "search"),
                                            from, hasFrom, lowering_.builder_.getInt32(mode)});
    lowering_.refCounts().releaseValue(values[0], element);
    if (mode == 2) {
      result = lowering_.builder_.CreateICmpNE(result, lowering_.builder_.getInt32(0));
    }
    break;
  }
  case Builtin::ArrJoin:
  case Builtin::ArrToString: {
    if (call->args.empty()) {
      llvm::Value *none = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
      result = lowering_.stringLowerer().callReturningString(
          lowering_.rt_.arrJoin,
          {array.value, none, lowering_.builder_.getInt64(0), lowering_.builder_.getInt32(0)});
    } else {
      Lowering::Operand separator = lowering_.refCounts().emitOperand(call->args[0], true);
      result = lowering_.stringLowerer().callReturningString(
          lowering_.rt_.arrJoin,
          {array.value, lowering_.stringLowerer().stringObj(separator.value),
           lowering_.stringLowerer().stringMeta(separator.value), lowering_.builder_.getInt32(1)});
      lowering_.refCounts().dropOperand(separator);
    }
    break;
  }
  case Builtin::ArrReverse:
    lowering_.builder_.CreateCall(lowering_.rt_.arrReverse, {array.value});
    result = returnArray();
    break;
  case Builtin::ArrSort:
    lowering_.builder_.CreateCall(lowering_.rt_.arrSort, {array.value});
    result = returnArray();
    break;
  case Builtin::ArrFill: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    auto [start, hasStart] = position(values, 1);
    auto [end, hasEnd] = position(values, 2);
    lowering_.builder_.CreateCall(lowering_.rt_.arrFill,
                                  {array.value, lowering_.functionFrame().spill(values[0], "fill"),
                                   start, end, hasStart, hasEnd});
    lowering_.refCounts().releaseValue(values[0], element);
    result = returnArray();
    break;
  }
  case Builtin::ArrSplice: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    auto [count, hasCount] = position(values, 1);
    size_t inserted = values.size() > 2 ? values.size() - 2 : 0;
    llvm::Value *slotsOut =
        lowering_.functionFrame().createStackSlot(lowering_.builder_.getPtrTy(), "splice.slots");
    result = lowering_.builder_.CreateCall(
        lowering_.rt_.arrSplice,
        {array.value, values[0], count, hasCount, lowering_.builder_.getInt64(inserted), slotsOut},
        "removed");
    llvm::Value *slots = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slotsOut);
    for (size_t i = 0; i < inserted; ++i) {
      lowering_.builder_.CreateStore(
          values[i + 2],
          lowering_.arrayLowerer().elementAddress(slots, element, lowering_.builder_.getInt64(i)));
      if (lowering_.memory().refMayBeMember(arrayType)) {
        lowering_.storeLowerer().groupStore(array.value, values[i + 2], element);
      }
    }
    break;
  }
  default:
    llvm_unreachable("not an array method");
  }
  lowering_.refCounts().dropOperand(array);

  return result;
}

llvm::Value *ArrayMethodLowerer::emitValueToString(llvm::Value *value, const Type *type) {
  return lowering_.stringLowerer().callReturningString(
      lowering_.rt_.valueToString, {lowering_.typeDescriptors().typeDescriptor(type),
                                    lowering_.functionFrame().spill(value, "value")});
}

}
