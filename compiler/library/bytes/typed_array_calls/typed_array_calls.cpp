#include "compiler/library/bytes/typed_array_calls/typed_array_calls.h"
#include "compiler/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
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

llvm::Value *TypedArrayCalls::emit() {
  switch (call->builtin) {
  case Builtin::BytesByteLength: {
    Lowering::Operand self = lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *length = lowering_.bytesLowerer().bytesLength(self.value);
    auto sizeOf = [](const Type *t) { return t->isTypedArray() ? t->elementSize() : 1u; };
    llvm::Value *size = lowering_.builder_.getInt64(sizeOf(self.type));
    if (self.type->isUnion()) {
      // Of the member the value is, by its class number, where elements of
      // more than a byte are among them.
      llvm::Value *id = nullptr;
      for (const Type *m : self.type->members()) {
        if (sizeOf(m) == 1) {
          continue;
        }
        if (!id) {
          id = lowering_.unionLowerer().objectClassId(self.value);
        }
        size = lowering_.builder_.CreateSelect(
            lowering_.builder_.CreateICmpEQ(
                id, lowering_.builder_.getInt32(lowering_.unionLowerer().plainTypeId(m))),
            lowering_.builder_.getInt64(sizeOf(m)), size);
      }
    }
    lowering_.refCounts().dropOperand(self);
    return lowering_.builder_.CreateTrunc(lowering_.builder_.CreateMul(length, size), i32);
  }
  case Builtin::BytesByteOffset: {
    Lowering::Operand self = lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    if (!lowering_.bytesTy_) {
      lowering_.bytesTy_ = bytesStruct(lowering_.ctx_);
    }
    llvm::Value *offset = lowering_.builder_.CreateLoad(
        i32, lowering_.builder_.CreateStructGEP(lowering_.bytesTy_, self.value, kOffset), "offset");
    lowering_.refCounts().dropOperand(self);
    return offset;
  }
  case Builtin::BytesBuffer: {
    Lowering::Operand self = lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *buffer =
        lowering_.builder_.CreateCall(fn("tz_bytes_buffer", ptr, {ptr}), {self.value});
    lowering_.refCounts().dropOperand(self);
    return buffer;
  }
  case Builtin::BytesAt: {
    Lowering::Operand self = receiver(0);
    llvm::Value *at = lowering_.exprLowerer().emitExpr(call->args[0]);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *length = lowering_.bytesLowerer().bytesLength(self.value);
    at = lowering_.builder_.CreateSelect(
        lowering_.builder_.CreateICmpSLT(at, lowering_.builder_.getInt64(0)),
        lowering_.builder_.CreateAdd(at, length), at);
    const Type *bytesType = self.type;
    auto read = [&] {
      return lowering_.bytesLowerer().loadElement(lowering_.bytesLowerer().bytesData(self.value),
                                                  bytesType, at);
    };
    llvm::Value *result = call->mayBeOutOfRange
                              ? lowering_.arrayLowerer().emitCheckedRead(
                                    lowering_.builder_.CreateICmpULT(at, length), type, read)
                              : read();
    lowering_.refCounts().dropOperand(self);
    return result;
  }
  case Builtin::BytesIndexOf: {
    Lowering::Operand self = receiver(1);
    llvm::Value *value = lowering_.exprLowerer().emitExpr(call->args[1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(2, 1, values);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *mode = lowering_.builder_.getInt32(uint32_t(constantOf(call->args[0])));
    llvm::Value *found =
        lowering_.builder_.CreateCall(fn("tz_bytes_index_of", i32, {ptr, f64, i64, i32, i32}),
                                      {self.value, value, values[0], has, mode});
    lowering_.refCounts().dropOperand(self);
    return constantOf(call->args[0]) == 2
               ? lowering_.builder_.CreateICmpNE(found, lowering_.builder_.getInt32(0))
               : found;
  }
  case Builtin::BytesJoin: {
    Lowering::Operand self = receiver(0);
    llvm::Value *text;
    if (call->args.empty()) {
      lowering_.throwLowerer().takeOperand(self);
      llvm::Value *none = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
      text = lowering_.stringLowerer().callReturningString(
          fn("tz_bytes_join", voidTy, {ptr, ptr, ptr, i64, i32}),
          {self.value, none, lowering_.builder_.getInt64(0), lowering_.builder_.getInt32(0)});
    } else {
      Lowering::Operand separator =
          lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      lowering_.throwLowerer().takeOperand(self);
      text = lowering_.stringLowerer().callReturningString(
          fn("tz_bytes_join", voidTy, {ptr, ptr, ptr, i64, i32}),
          {self.value, lowering_.stringLowerer().stringObj(separator.value),
           lowering_.stringLowerer().stringMeta(separator.value), lowering_.builder_.getInt32(1)});
      lowering_.refCounts().dropOperand(separator);
    }
    lowering_.refCounts().dropOperand(self);
    return text;
  }
  case Builtin::BytesReverse:
  case Builtin::BytesSort: {
    Lowering::Operand self = receiver(0);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.builder_.CreateCall(
        fn(call->builtin == Builtin::BytesSort ? "tz_bytes_sort" : "tz_bytes_reverse", voidTy,
           {ptr}),
        {self.value});
    return returnSelf(self);
  }
  case Builtin::BytesFill: {
    Lowering::Operand self = receiver(0);
    llvm::Value *value = lowering_.exprLowerer().emitExpr(call->args[0]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 2, values);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.builder_.CreateCall(fn("tz_bytes_fill", voidTy, {ptr, f64, i64, i64, i32}),
                                  {self.value, value, values[0], values[1], has});
    return returnSelf(self);
  }
  case Builtin::BytesSet: {
    Lowering::Operand self = receiver(0);
    Expr *source = call->args[0];
    Lowering::Operand from = lowering_.refCounts().emitOperand(
        source, call->args.size() < 2 || AssignLowerer::cannotAssign(call->args[1]));
    lowering_.throwLowerer().holdOperand(from, call->args.size() > 1 &&
                                                   lowering_.mayThrow().exprThrows(call->args[1]));
    llvm::Value *offset = call->args.size() > 1 ? lowering_.exprLowerer().emitExpr(call->args[1])
                                                : lowering_.builder_.getInt64(0);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.throwLowerer().takeOperand(from);
    lowering_.builder_.CreateCall(
        fn(source->type->isArray() ? "tz_bytes_set_array" : "tz_bytes_set_bytes", voidTy,
           {ptr, ptr, i64}),
        {self.value, from.value, offset});
    lowering_.refCounts().dropOperand(from);
    lowering_.refCounts().dropOperand(self);
    lowering_.throwLowerer().emitThrowCheck(); // RangeError: offset is out of bounds
    return nullptr;
  }
  case Builtin::BytesSubarray:
  case Builtin::BytesSlice:
  case Builtin::ArrayBufferSlice: {
    Lowering::Operand self = receiver(0);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(0, 2, values);
    lowering_.throwLowerer().takeOperand(self);
    llvm::StringRef name = call->builtin == Builtin::BytesSubarray ? "tz_bytes_subarray"
                           : call->builtin == Builtin::BytesSlice  ? "tz_bytes_slice"
                                                                   : "tz_abuf_slice";
    llvm::Value *made = lowering_.builder_.CreateCall(fn(name, ptr, {ptr, i64, i64, i32}),
                                                      {self.value, values[0], values[1], has});
    lowering_.refCounts().dropOperand(self);
    return made;
  }
  case Builtin::BytesCopyWithin: {
    Lowering::Operand self = receiver(0);
    std::vector<llvm::Value *> values;
    for (size_t i = 0; i < 2; ++i) {
      values.push_back(lowering_.exprLowerer().emitExpr(call->args[i]));
    }
    llvm::Value *end = call->args.size() > 2 ? lowering_.exprLowerer().emitExpr(call->args[2])
                                             : lowering_.builder_.getInt64(0);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.builder_.CreateCall(fn("tz_bytes_copy_within", voidTy, {ptr, i64, i64, i64, i32}),
                                  {self.value, values[0], values[1], end,
                                   lowering_.builder_.getInt32(call->args.size() > 2)});
    return returnSelf(self);
  }
  case Builtin::BytesToReversed:
  case Builtin::BytesToSorted: {
    Lowering::Operand self = receiver(0);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *made = lowering_.builder_.CreateCall(
        fn(call->builtin == Builtin::BytesToSorted ? "tz_bytes_to_sorted" : "tz_bytes_to_reversed",
           ptr, {ptr}),
        {self.value});
    lowering_.refCounts().dropOperand(self);
    return made;
  }
  case Builtin::BytesWith: {
    Lowering::Operand self = receiver(0);
    llvm::Value *index = lowering_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *value = lowering_.exprLowerer().emitExpr(call->args[1]);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *made = lowering_.builder_.CreateCall(fn("tz_bytes_with", ptr, {ptr, i64, f64}),
                                                      {self.value, index, value});
    lowering_.refCounts().dropOperand(self);
    lowering_.throwLowerer().emitThrowCheck(); // RangeError: Invalid typed array index
    return made;
  }
  case Builtin::ArrayBufferIsView: {
    lowering_.stmtLowerer().emitDiscarded(call->args[0]);
    return lowering_.builder_.getInt1(call->args[0]->type->nonOptional()->isByteView());
  }
  default:
    llvm_unreachable("not a call of TypedArrayCalls");
  }
}

} // namespace tz
