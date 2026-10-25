#include "compiler/lower/values/array_method_lowerer/array_method_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// Header fields; must match runtime/core/array/array.c.
enum ArrayField : unsigned { kRefCount, kElementType, kData, kLength, kCapacity, kAlloc };

} // namespace

ArrayMethodLowerer::ArrayMethodLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *ArrayMethodLowerer::emitArrayMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  CodeGen::Operand array = codegen_.refCounts().emitOperand(member->object, llvm::all_of(call->args, AssignLowerer::cannotAssign));

  // All arguments are evaluated before the method runs, as in JavaScript.
  auto ownedArgs = [&](size_t from) {
    codegen_.throwLowerer().holdOperand(array, llvm::any_of(call->args, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));
    std::vector<llvm::Value *> values =
        codegen_.throwLowerer().emitHeldValues(llvm::ArrayRef<Expr *>(call->args).drop_front(from));
    codegen_.throwLowerer().takeOperand(array);

    return values;
  };
  // An optional position argument: its value and whether it was given.
  auto position = [&](llvm::ArrayRef<llvm::Value *> values,
                      size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < values.size()) {
      return {values[i], codegen_.builder_.getInt32(1)};
    }

    return {codegen_.builder_.getInt64(0), codegen_.builder_.getInt32(0)};
  };
  // Methods that return the array itself hand over the receiver's reference.
  auto returnArray = [&] {
    if (!array.owned) {
      codegen_.refCounts().retainValue(array.value, arrayType);
    }
    array.owned = false;

    return array.value;
  };

  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::ArrPush: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    // `a.push(x, ...b)`: the arrays spread are appended where they stand.
    std::vector<llvm::Value *> run;
    for (size_t i = 0; i < values.size(); ++i) {
      if (!call->spreads(i)) {
        run.push_back(values[i]);
        continue;
      }
      if (!run.empty()) {
        codegen_.arrayLowerer().emitPush(array.value, run, element);
      }
      run.clear();
      codegen_.builder_.CreateCall(codegen_.rt_.arrAppend, {array.value, values[i]});
      codegen_.refCounts().releaseValue(values[i], arrayType);
    }
    if (!run.empty()) {
      codegen_.arrayLowerer().emitPush(array.value, run, element);
    }
    result = codegen_.builder_.CreateTrunc(codegen_.arrayLowerer().arrayLength(array.value), codegen_.builder_.getInt32Ty());
    break;
  }
  case Builtin::ArrUnshift: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    llvm::Value *slots =
        codegen_.builder_.CreateCall(codegen_.rt_.arrUnshiftSlots, {array.value, codegen_.builder_.getInt64(values.size())});
    for (size_t i = 0; i < values.size(); ++i) {
      codegen_.builder_.CreateStore(values[i], codegen_.arrayLowerer().elementAddress(slots, element, codegen_.builder_.getInt64(i)));
      if (codegen_.memoryAnalysis().refMayBeMember(arrayType)) {
        codegen_.storeLowerer().groupStore(array.value, values[i], element);
      }
    }
    result = codegen_.builder_.CreateTrunc(codegen_.arrayLowerer().arrayLength(array.value), codegen_.builder_.getInt32Ty());
    break;
  }
  case Builtin::ArrPop: {
    // The element moves out of the array: no counting needed.
    llvm::Value *length = codegen_.arrayLowerer().arrayLength(array.value);
    auto pop = [&] {
      llvm::Value *last = codegen_.builder_.CreateSub(length, codegen_.builder_.getInt64(1));
      llvm::Value *value = codegen_.builder_.CreateLoad(
          codegen_.moduleBuilder().lower(element), codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(array.value), element, last));
      codegen_.builder_.CreateStore(codegen_.builder_.CreateTrunc(last, codegen_.builder_.getInt32Ty()),
                           codegen_.arrayLowerer().arrayField(array.value, kLength));
      // Out of a member, a reference inside its group counts again.
      if (codegen_.memoryAnalysis().refMayBeMember(arrayType)) {
        codegen_.storeLowerer().takeFrom(array.value, value, element);
      }

      return value;
    };
    result =
        call->mayBeOutOfRange
            ? codegen_.arrayLowerer().emitCheckedRead(codegen_.builder_.CreateICmpNE(length, codegen_.builder_.getInt64(0)), call->type, pop)
            : pop();
    break;
  }
  case Builtin::ArrShift: {
    llvm::Value *slot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(element), "shifted");
    llvm::Value *has = codegen_.builder_.CreateCall(codegen_.rt_.arrShift, {array.value, slot});
    llvm::Value *value = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), slot);
    // Out of a member, a reference inside its group counts again.
    if (codegen_.memoryAnalysis().refMayBeMember(arrayType)) {
      codegen_.storeLowerer().takeFrom(array.value, value, element);
    }
    result = call->mayBeOutOfRange
                 ? codegen_.optionalLowerer().makeOptional(value, codegen_.builder_.CreateICmpNE(has, codegen_.builder_.getInt32(0)), call->type)
                 : value;
    break;
  }
  case Builtin::ArrAt: {
    llvm::Value *length = codegen_.arrayLowerer().arrayLength(array.value);
    llvm::Value *at = codegen_.exprLowerer().emitExpr(call->args[0]);
    at = codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpSLT(at, codegen_.builder_.getInt64(0)),
                               codegen_.builder_.CreateAdd(at, length), at);
    auto read = [&] {
      llvm::Value *value =
          codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(array.value), element, at));
      codegen_.refCounts().retainValue(value, element);

      return value;
    };
    result = call->mayBeOutOfRange
                 ? codegen_.arrayLowerer().emitCheckedRead(codegen_.builder_.CreateICmpULT(at, length), call->type, read)
                 : read();
    break;
  }
  case Builtin::ArrSlice: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    llvm::Value *start = values.empty() ? codegen_.builder_.getInt64(0) : values[0];
    auto [end, hasEnd] = position(values, 1);
    // A copy of a type that may form cycles is a member unless nothing
    // stores it, whatever the array copied is.
    llvm::FunctionCallee slice = codegen_.rt_.arrSlice;
    if (codegen_.isCyclic(call->type)) {
      slice =
          codegen_.groupLowerer().groupFunction(codegen_.makesMember(call, call->type) ? "tz_arr_slice_group" : "tz_arr_slice_plain",
                        codegen_.builder_.getPtrTy(),
                        {codegen_.builder_.getPtrTy(), codegen_.builder_.getInt64Ty(), codegen_.builder_.getInt64Ty(),
                         codegen_.builder_.getInt32Ty()});
    }
    result = codegen_.builder_.CreateCall(slice, {array.value, start, end, hasEnd}, "slice");
    codegen_.groupLowerer().noteUnstored(result, call);
    break;
  }
  case Builtin::ArrConcat: {
    std::vector<CodeGen::Operand> parts;
    for (size_t i = 0; i < call->args.size(); ++i) {
      parts.push_back({codegen_.exprLowerer().emitExpr(call->args[i]), true, call->args[i]->type});
    }
    llvm::FunctionCallee copy = codegen_.rt_.arrCopy;
    if (codegen_.isCyclic(call->type)) {
      copy =
          codegen_.groupLowerer().groupFunction(codegen_.makesMember(call, call->type) ? "tz_arr_copy_group" : "tz_arr_copy_plain",
                        codegen_.builder_.getPtrTy(), {codegen_.builder_.getPtrTy()});
    }
    result = codegen_.builder_.CreateCall(copy, {array.value}, "concat");
    codegen_.groupLowerer().noteUnstored(result, call);
    for (CodeGen::Operand &part : parts) {
      if (part.type == arrayType) {
        codegen_.builder_.CreateCall(codegen_.rt_.arrAppend, {result, part.value});
        codegen_.refCounts().dropOperand(part);
      } else {
        codegen_.arrayLowerer().emitPush(result, {part.value}, element); // the array takes ownership
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
    result = codegen_.builder_.CreateCall(codegen_.rt_.arrIndexOf, {array.value, codegen_.functionFrame().spill(values[0], "search"), from,
                                                  hasFrom, codegen_.builder_.getInt32(mode)});
    codegen_.refCounts().releaseValue(values[0], element);
    if (mode == 2) {
      result = codegen_.builder_.CreateICmpNE(result, codegen_.builder_.getInt32(0));
    }
    break;
  }
  case Builtin::ArrJoin:
  case Builtin::ArrToString: {
    if (call->args.empty()) {
      llvm::Value *none = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
      result = codegen_.stringLowerer().callReturningString(codegen_.rt_.arrJoin,
                                   {array.value, none, codegen_.builder_.getInt64(0), codegen_.builder_.getInt32(0)});
    } else {
      CodeGen::Operand separator = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      result =
          codegen_.stringLowerer().callReturningString(codegen_.rt_.arrJoin, {array.value, codegen_.stringLowerer().stringObj(separator.value),
                                            codegen_.stringLowerer().stringMeta(separator.value), codegen_.builder_.getInt32(1)});
      codegen_.refCounts().dropOperand(separator);
    }
    break;
  }
  case Builtin::ArrReverse:
    codegen_.builder_.CreateCall(codegen_.rt_.arrReverse, {array.value});
    result = returnArray();
    break;
  case Builtin::ArrSort:
    codegen_.builder_.CreateCall(codegen_.rt_.arrSort, {array.value});
    result = returnArray();
    break;
  case Builtin::ArrFill: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    auto [start, hasStart] = position(values, 1);
    auto [end, hasEnd] = position(values, 2);
    codegen_.builder_.CreateCall(codegen_.rt_.arrFill,
                        {array.value, codegen_.functionFrame().spill(values[0], "fill"), start, end, hasStart, hasEnd});
    codegen_.refCounts().releaseValue(values[0], element);
    result = returnArray();
    break;
  }
  case Builtin::ArrSplice: {
    // The runtime moves the removed elements into a new array and opens a
    // gap for the inserted ones, which take over the values.
    std::vector<llvm::Value *> values = ownedArgs(0);
    auto [count, hasCount] = position(values, 1);
    size_t inserted = values.size() > 2 ? values.size() - 2 : 0;
    llvm::Value *slotsOut = codegen_.functionFrame().createStackSlot(codegen_.builder_.getPtrTy(), "splice.slots");
    result = codegen_.builder_.CreateCall(
        codegen_.rt_.arrSplice,
        {array.value, values[0], count, hasCount, codegen_.builder_.getInt64(inserted), slotsOut},
        "removed");
    llvm::Value *slots = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slotsOut);
    for (size_t i = 0; i < inserted; ++i) {
      codegen_.builder_.CreateStore(values[i + 2], codegen_.arrayLowerer().elementAddress(slots, element, codegen_.builder_.getInt64(i)));
      if (codegen_.memoryAnalysis().refMayBeMember(arrayType)) {
        codegen_.storeLowerer().groupStore(array.value, values[i + 2], element);
      }
    }
    break;
  }
  default:
    llvm_unreachable("not an array method");
  }
  codegen_.refCounts().dropOperand(array);

  return result;
}

llvm::Value *ArrayMethodLowerer::emitValueToString(llvm::Value *value, const Type *type) {
  return codegen_.stringLowerer().callReturningString(codegen_.rt_.valueToString, {codegen_.typeDescriptors().typeDescriptor(type), codegen_.functionFrame().spill(value, "value")});
}

} // namespace tz
