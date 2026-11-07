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

using llvm::cast;

namespace tz {

namespace {

enum ArrayField : unsigned { kRefCount, kElementType, kData, kLength, kCapacity, kAlloc };

}

ArrayMethodLowerer::ArrayMethodLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *ArrayMethodLowerer::emitArrayMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  Lowering::Operand array = this->lowering.refCounts().emitOperand(
      member->object, llvm::all_of(call->args, AssignLowerer::cannotAssign));

  auto ownedArgs = [&](size_t from) {
    std::vector<llvm::Value *> values =
        this->lowering.exprLowerer().emitExprs(llvm::ArrayRef<Expr *>(call->args).drop_front(from));

    return values;
  };
  auto position = [&](llvm::ArrayRef<llvm::Value *> values,
                      size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < values.size()) {
      return {values[i], this->lowering.builder_.getInt32(1)};
    }

    return {this->lowering.builder_.getInt64(0), this->lowering.builder_.getInt32(0)};
  };
  auto returnArray = [&] {
    if (!array.owned) {
      this->lowering.refCounts().retainValue(array.value, arrayType);
    }
    array.owned = false;

    return array.value;
  };

  llvm::Value *result = nullptr;
  switch (call->builtin()) {
    case Builtin::ArrPush: {
      std::vector<llvm::Value *> values = ownedArgs(0);
      std::vector<llvm::Value *> run;
      for (size_t i = 0; i < values.size(); ++i) {
        if (!call->spreads(i)) {
          run.push_back(values[i]);
          continue;
        }
        if (!run.empty()) {
          this->lowering.arrayLowerer().emitPush(array.value, run, element);
        }
        run.clear();
        this->lowering.builder_.CreateCall(this->lowering.rt_.arrAppend, {array.value, values[i]});
        this->lowering.refCounts().releaseValue(values[i], arrayType);
      }
      if (!run.empty()) {
        this->lowering.arrayLowerer().emitPush(array.value, run, element);
      }
      result = this->lowering.arrayLowerer().arrayLength(array.value);
      break;
    }
    case Builtin::ArrUnshift: {
      std::vector<llvm::Value *> values = ownedArgs(0);
      llvm::Value *slots = this->lowering.builder_.CreateCall(
          this->lowering.rt_.arrUnshiftSlots,
          {array.value, this->lowering.builder_.getInt64(values.size())});
      for (size_t i = 0; i < values.size(); ++i) {
        this->lowering.storeLowerer().storeBarrier(array.value, values[i], element);
        this->lowering.builder_.CreateStore(
            values[i], this->lowering.arrayLowerer().elementAddress(
                           slots, element, this->lowering.builder_.getInt64(i)));
        if (this->lowering.memory().refMayCarryMember(arrayType)) {
          this->lowering.storeLowerer().groupStore(array.value, values[i], element);
        }
      }
      result = this->lowering.arrayLowerer().arrayLength(array.value);
      break;
    }
    case Builtin::ArrPop: {
      llvm::Value *length = this->lowering.arrayLowerer().arrayLength(array.value);
      auto pop = [&] {
        llvm::Value *last =
            this->lowering.builder_.CreateSub(length, this->lowering.builder_.getInt64(1));
        llvm::Value *value = this->lowering.builder_.CreateLoad(
            this->lowering.moduleBuilder().lower(element),
            this->lowering.arrayLowerer().elementAddress(
                this->lowering.arrayLowerer().arrayData(array.value), element, last));
        this->lowering.builder_.CreateStore(
            this->lowering.builder_.CreateTrunc(last, this->lowering.builder_.getInt32Ty()),
            this->lowering.arrayLowerer().arrayField(array.value, kLength));
        if (this->lowering.memory().refMayCarryMember(arrayType)) {
          this->lowering.storeLowerer().takeFrom(array.value, value, element);
        }

        return value;
      };
      result = call->mayBeOutOfRange ? this->lowering.arrayLowerer().emitCheckedRead(
                                           this->lowering.builder_.CreateICmpNE(
                                               length, this->lowering.builder_.getInt64(0)),
                                           call->type, pop)
                                     : pop();
      break;
    }
    case Builtin::ArrShift: {
      llvm::Value *slot = this->lowering.functionFrame().createStackSlot(
          this->lowering.moduleBuilder().lower(element), "shifted");
      llvm::Value *has =
          this->lowering.builder_.CreateCall(this->lowering.rt_.arrShift, {array.value, slot});
      llvm::Value *value =
          this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(element), slot);
      if (this->lowering.memory().refMayCarryMember(arrayType)) {
        this->lowering.storeLowerer().takeFrom(array.value, value, element);
      }
      result =
          call->mayBeOutOfRange
              ? this->lowering.optionalLowerer().makeOptional(
                    value,
                    this->lowering.builder_.CreateICmpNE(has, this->lowering.builder_.getInt32(0)),
                    call->type)
              : value;
      break;
    }
    case Builtin::ArrAt: {
      llvm::Value *length = this->lowering.arrayLowerer().arrayLength(array.value);
      llvm::Value *at = this->lowering.exprLowerer().emitExpr(call->args[0]);
      at = this->lowering.builder_.CreateSelect(
          this->lowering.builder_.CreateICmpSLT(at, this->lowering.builder_.getInt64(0)),
          this->lowering.builder_.CreateAdd(at, length), at);
      auto read = [&] {
        llvm::Value *value = this->lowering.builder_.CreateLoad(
            this->lowering.moduleBuilder().lower(element),
            this->lowering.arrayLowerer().elementAddress(
                this->lowering.arrayLowerer().arrayData(array.value), element, at));
        this->lowering.refCounts().retainValue(value, element);

        return value;
      };
      result = call->mayBeOutOfRange
                   ? this->lowering.arrayLowerer().emitCheckedRead(
                         this->lowering.builder_.CreateICmpULT(at, length), call->type, read)
                   : read();
      break;
    }
    case Builtin::ArrSlice: {
      std::vector<llvm::Value *> values = ownedArgs(0);
      llvm::Value *start = values.empty() ? this->lowering.builder_.getInt64(0) : values[0];
      auto [end, hasEnd] = position(values, 1);
      llvm::FunctionCallee slice = this->lowering.rt_.arrSlice;
      if (this->lowering.isCyclic(call->type)) {
        slice = this->lowering.groupLowerer().groupFunction(
            this->lowering.makesMember(call, call->type) ? "tz_arr_slice_group"
                                                         : "tz_arr_slice_plain",
            this->lowering.builder_.getPtrTy(),
            {this->lowering.builder_.getPtrTy(), this->lowering.builder_.getInt64Ty(),
             this->lowering.builder_.getInt64Ty(), this->lowering.builder_.getInt32Ty()});
      }
      result =
          this->lowering.builder_.CreateCall(slice, {array.value, start, end, hasEnd}, "slice");
      this->lowering.groupLowerer().noteUnstored(result, call);
      break;
    }
    case Builtin::ArrConcat: {
      std::vector<Lowering::Operand> parts;
      for (size_t i = 0; i < call->args.size(); ++i) {
        parts.push_back(
            {this->lowering.exprLowerer().emitExpr(call->args[i]), true, call->args[i]->type});
      }
      llvm::FunctionCallee copy = this->lowering.rt_.arrCopy;
      if (this->lowering.isCyclic(call->type)) {
        copy = this->lowering.groupLowerer().groupFunction(
            this->lowering.makesMember(call, call->type) ? "tz_arr_copy_group"
                                                         : "tz_arr_copy_plain",
            this->lowering.builder_.getPtrTy(), {this->lowering.builder_.getPtrTy()});
      }
      result = this->lowering.builder_.CreateCall(copy, {array.value}, "concat");
      this->lowering.groupLowerer().noteUnstored(result, call);
      for (Lowering::Operand &part : parts) {
        if (part.type == arrayType) {
          this->lowering.builder_.CreateCall(this->lowering.rt_.arrAppend, {result, part.value});
          this->lowering.refCounts().dropOperand(part);
        } else {
          this->lowering.arrayLowerer().emitPush(result, {part.value}, element);
        }
      }
      break;
    }
    case Builtin::ArrIndexOf:
    case Builtin::ArrLastIndexOf:
    case Builtin::ArrIncludes: {
      std::vector<llvm::Value *> values = ownedArgs(0);
      auto [from, hasFrom] = position(values, 1);
      int mode = call->builtin() == Builtin::ArrIndexOf       ? 0
                 : call->builtin() == Builtin::ArrLastIndexOf ? 1
                                                              : 2;
      result = this->lowering.builder_.CreateCall(
          this->lowering.rt_.arrIndexOf,
          {array.value, this->lowering.functionFrame().spill(values[0], "search"), from, hasFrom,
           this->lowering.builder_.getInt32(mode)});
      this->lowering.refCounts().releaseValue(values[0], element);
      result =
          mode == 2
              ? this->lowering.builder_.CreateICmpNE(result, this->lowering.builder_.getInt32(0))
              : this->lowering.builder_.CreateSExt(result, this->lowering.builder_.getInt64Ty());
      break;
    }
    case Builtin::ArrJoin:
    case Builtin::ArrToString: {
      if (call->args.empty()) {
        llvm::Value *none = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
        result = this->lowering.stringLowerer().callReturningString(
            this->lowering.rt_.arrJoin, {array.value, none, this->lowering.builder_.getInt64(0),
                                         this->lowering.builder_.getInt32(0)});
      } else {
        Lowering::Operand separator = this->lowering.refCounts().emitOperand(call->args[0], true);
        result = this->lowering.stringLowerer().callReturningString(
            this->lowering.rt_.arrJoin,
            {array.value, this->lowering.stringLowerer().stringObj(separator.value),
             this->lowering.stringLowerer().stringMeta(separator.value),
             this->lowering.builder_.getInt32(1)});
        this->lowering.refCounts().dropOperand(separator);
      }
      break;
    }
    case Builtin::ArrReverse:
      this->lowering.builder_.CreateCall(this->lowering.rt_.arrReverse, {array.value});
      result = returnArray();
      break;
    case Builtin::ArrSort:
      this->lowering.builder_.CreateCall(this->lowering.rt_.arrSort, {array.value});
      result = returnArray();
      break;
    case Builtin::ArrFill: {
      std::vector<llvm::Value *> values = ownedArgs(0);
      auto [start, hasStart] = position(values, 1);
      auto [end, hasEnd] = position(values, 2);
      this->lowering.builder_.CreateCall(this->lowering.rt_.arrFill,
                                         {array.value,
                                          this->lowering.functionFrame().spill(values[0], "fill"),
                                          start, end, hasStart, hasEnd});
      this->lowering.refCounts().releaseValue(values[0], element);
      result = returnArray();
      break;
    }
    case Builtin::ArrSplice: {
      std::vector<llvm::Value *> values = ownedArgs(0);
      auto [count, hasCount] = position(values, 1);
      size_t inserted = values.size() > 2 ? values.size() - 2 : 0;
      llvm::Value *slotsOut = this->lowering.functionFrame().createStackSlot(
          this->lowering.builder_.getPtrTy(), "splice.slots");
      result =
          this->lowering.builder_.CreateCall(this->lowering.rt_.arrSplice,
                                             {array.value, values[0], count, hasCount,
                                              this->lowering.builder_.getInt64(inserted), slotsOut},
                                             "removed");
      llvm::Value *slots =
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slotsOut);
      for (size_t i = 0; i < inserted; ++i) {
        this->lowering.builder_.CreateStore(
            values[i + 2], this->lowering.arrayLowerer().elementAddress(
                               slots, element, this->lowering.builder_.getInt64(i)));
        if (this->lowering.memory().refMayCarryMember(arrayType)) {
          this->lowering.storeLowerer().groupStore(array.value, values[i + 2], element);
        }
      }
      break;
    }
    default:
      llvm_unreachable("not an array method");
  }
  this->lowering.refCounts().dropOperand(array);

  return result;
}

llvm::Value *ArrayMethodLowerer::emitValueToString(llvm::Value *value, const Type *type) {
  return this->lowering.stringLowerer().callReturningString(
      this->lowering.rt_.valueToString, {this->lowering.typeDescriptors().typeDescriptor(type),
                                         this->lowering.functionFrame().spill(value, "value")});
}

}
