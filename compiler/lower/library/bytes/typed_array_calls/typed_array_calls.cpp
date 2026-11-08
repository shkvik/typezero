#include "compiler/lower/library/bytes/typed_array_calls/typed_array_calls.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/lower/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
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
  switch (call->builtin()) {
    case Builtin::BytesByteLength: {
      Lowering::Operand self = this->lowering.refCounts().emitOperand(member->object, true);
      llvm::Value *length = this->lowering.bytesLowerer().bytesLength(self.value);
      auto sizeOf = [](const Type *t) { return t->isTypedArray() ? t->elementSize() : 1u; };
      llvm::Value *size = this->lowering.builder.getInt64(sizeOf(self.type));
      if (self.type->isUnion()) {
        llvm::Value *id = nullptr;
        for (const Type *m : self.type->members()) {
          if (sizeOf(m) == 1) {
            continue;
          }
          if (!id) {
            id = this->lowering.unionLowerer().objectClassId(self.value);
          }
          size = this->lowering.builder.CreateSelect(
              this->lowering.builder.CreateICmpEQ(
                  id,
                  this->lowering.builder.getInt32(this->lowering.unionLowerer().plainTypeId(m))),
              this->lowering.builder.getInt64(sizeOf(m)), size);
        }
      }
      this->lowering.refCounts().dropOperand(self);
      return this->lowering.builder.CreateMul(length, size);
    }
    case Builtin::BytesByteOffset: {
      Lowering::Operand self = this->lowering.refCounts().emitOperand(member->object, true);
      if (!this->lowering.bytesTy) {
        this->lowering.bytesTy = BytesLowerer::bytesStruct(this->lowering.ctx);
      }
      llvm::Value *offset = this->lowering.builder.CreateLoad(
          i32, this->lowering.builder.CreateStructGEP(this->lowering.bytesTy, self.value, kOffset),
          "offset");
      this->lowering.refCounts().dropOperand(self);
      return this->lowering.builder.CreateZExt(offset, i64);
    }
    case Builtin::BytesBuffer: {
      Lowering::Operand self = this->lowering.refCounts().emitOperand(member->object, true);
      llvm::Value *buffer =
          this->lowering.builder.CreateCall(this->fn("tz_bytes_buffer", ptr, {ptr}), {self.value});
      this->lowering.refCounts().dropOperand(self);
      return buffer;
    }
    case Builtin::BytesAt: {
      Lowering::Operand self = this->receiver(0);
      llvm::Value *at = this->lowering.exprLowerer().emitExpr(call->args[0]);
      llvm::Value *length = this->lowering.bytesLowerer().bytesLength(self.value);
      at = this->lowering.builder.CreateSelect(
          this->lowering.builder.CreateICmpSLT(at, this->lowering.builder.getInt64(0)),
          this->lowering.builder.CreateAdd(at, length), at);
      const Type *bytesType = self.type;
      auto read = [&] {
        return this->lowering.bytesLowerer().loadElement(
            this->lowering.bytesLowerer().bytesData(self.value), bytesType, at);
      };
      llvm::Value *result = call->mayBeOutOfRange
                                ? this->lowering.arrayLowerer().emitCheckedRead(
                                      this->lowering.builder.CreateICmpULT(at, length), type, read)
                                : read();
      this->lowering.refCounts().dropOperand(self);
      return result;
    }
    case Builtin::BytesIndexOf: {
      Lowering::Operand self = this->receiver(1);
      llvm::Value *value = this->lowering.exprLowerer().emitExpr(call->args[1]);
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(2, 1, values);
      llvm::Value *mode =
          this->lowering.builder.getInt32(uint32_t(BytesCallSite::constantOf(call->args[0])));
      llvm::Value *found = this->lowering.builder.CreateCall(
          this->fn("tz_bytes_index_of", i32, {ptr, f64, i64, i32, i32}),
          {self.value, value, values[0], has, mode});
      this->lowering.refCounts().dropOperand(self);
      return BytesCallSite::constantOf(call->args[0]) == 2
                 ? this->lowering.builder.CreateICmpNE(found, this->lowering.builder.getInt32(0))
                 : this->lowering.builder.CreateSExt(found, i64);
    }
    case Builtin::BytesJoin: {
      Lowering::Operand self = this->receiver(0);
      llvm::Value *text;
      if (call->args.empty()) {
        llvm::Value *none = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
        text = this->lowering.stringLowerer().callReturningString(
            this->fn("tz_bytes_join", voidTy, {ptr, ptr, ptr, i64, i32}),
            {self.value, none, this->lowering.builder.getInt64(0),
             this->lowering.builder.getInt32(0)});
      } else {
        Lowering::Operand separator = this->lowering.refCounts().emitOperand(call->args[0], true);
        text = this->lowering.stringLowerer().callReturningString(
            this->fn("tz_bytes_join", voidTy, {ptr, ptr, ptr, i64, i32}),
            {self.value, this->lowering.stringLowerer().stringObj(separator.value),
             this->lowering.stringLowerer().stringMeta(separator.value),
             this->lowering.builder.getInt32(1)});
        this->lowering.refCounts().dropOperand(separator);
      }
      this->lowering.refCounts().dropOperand(self);
      return text;
    }
    case Builtin::BytesReverse:
    case Builtin::BytesSort: {
      Lowering::Operand self = this->receiver(0);
      this->lowering.builder.CreateCall(
          this->fn(call->builtin() == Builtin::BytesSort ? "tz_bytes_sort" : "tz_bytes_reverse",
                   voidTy, {ptr}),
          {self.value});
      return this->returnSelf(self);
    }
    case Builtin::BytesFill: {
      Lowering::Operand self = this->receiver(0);
      llvm::Value *value = this->lowering.exprLowerer().emitExpr(call->args[0]);
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(1, 2, values);
      this->lowering.builder.CreateCall(
          this->fn("tz_bytes_fill", voidTy, {ptr, f64, i64, i64, i32}),
          {self.value, value, values[0], values[1], has});
      return this->returnSelf(self);
    }
    case Builtin::BytesSet: {
      Lowering::Operand self = this->receiver(0);
      Expr *source = call->args[0];
      Lowering::Operand from = this->lowering.refCounts().emitOperand(
          source, call->args.size() < 2 || AssignLowerer::cannotAssign(call->args[1]));
      llvm::Value *offset = call->args.size() > 1
                                ? this->lowering.exprLowerer().emitExpr(call->args[1])
                                : this->lowering.builder.getInt64(0);
      this->lowering.builder.CreateCall(
          this->fn(source->type->isArray() ? "tz_bytes_set_array" : "tz_bytes_set_bytes", voidTy,
                   {ptr, ptr, i64}),
          {self.value, from.value, offset});
      this->lowering.refCounts().dropOperand(from);
      this->lowering.refCounts().dropOperand(self);
      return nullptr;
    }
    case Builtin::BytesSubarray:
    case Builtin::BytesSlice:
    case Builtin::ArrayBufferSlice: {
      Lowering::Operand self = this->receiver(0);
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(0, 2, values);
      llvm::StringRef name = call->builtin() == Builtin::BytesSubarray ? "tz_bytes_subarray"
                             : call->builtin() == Builtin::BytesSlice  ? "tz_bytes_slice"
                                                                       : "tz_abuf_slice";
      llvm::Value *made = this->lowering.builder.CreateCall(
          this->fn(name, ptr, {ptr, i64, i64, i32}), {self.value, values[0], values[1], has});
      this->lowering.refCounts().dropOperand(self);
      return made;
    }
    case Builtin::BytesCopyWithin: {
      Lowering::Operand self = this->receiver(0);
      std::vector<llvm::Value *> values;
      for (size_t i = 0; i < 2; ++i) {
        values.push_back(this->lowering.exprLowerer().emitExpr(call->args[i]));
      }
      llvm::Value *end = call->args.size() > 2
                             ? this->lowering.exprLowerer().emitExpr(call->args[2])
                             : this->lowering.builder.getInt64(0);
      this->lowering.builder.CreateCall(
          this->fn("tz_bytes_copy_within", voidTy, {ptr, i64, i64, i64, i32}),
          {self.value, values[0], values[1], end,
           this->lowering.builder.getInt32(call->args.size() > 2)});
      return this->returnSelf(self);
    }
    case Builtin::BytesToReversed:
    case Builtin::BytesToSorted: {
      Lowering::Operand self = this->receiver(0);
      llvm::Value *made = this->lowering.builder.CreateCall(
          this->fn(call->builtin() == Builtin::BytesToSorted ? "tz_bytes_to_sorted"
                                                             : "tz_bytes_to_reversed",
                   ptr, {ptr}),
          {self.value});
      this->lowering.refCounts().dropOperand(self);
      return made;
    }
    case Builtin::BytesWith: {
      Lowering::Operand self = this->receiver(0);
      llvm::Value *index = this->lowering.exprLowerer().emitExpr(call->args[0]);
      llvm::Value *value = this->lowering.exprLowerer().emitExpr(call->args[1]);
      llvm::Value *made = this->lowering.builder.CreateCall(
          this->fn("tz_bytes_with", ptr, {ptr, i64, f64}), {self.value, index, value});
      this->lowering.refCounts().dropOperand(self);
      return made;
    }
    case Builtin::ArrayBufferIsView: {
      this->lowering.stmtLowerer().emitDiscarded(call->args[0]);
      return this->lowering.builder.getInt1(call->args[0]->type->nonOptional()->isByteView());
    }
    default:
      llvm_unreachable("not a call of TypedArrayCalls");
  }
}

}
