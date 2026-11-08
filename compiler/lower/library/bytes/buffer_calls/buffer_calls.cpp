#include "compiler/lower/library/bytes/buffer_calls/buffer_calls.h"
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

llvm::Value *BufferCalls::emit() {
  switch (call->builtin()) {
    case Builtin::BufferFromString: {
      Lowering::Operand text = this->lowering.refCounts().emitOperand(
          call->args[0], AssignLowerer::cannotAssign(call->args[1]));
      llvm::Value *enc = this->encoding(call->args[1]);
      llvm::Value *made = this->lowering.builder.CreateCall(
          this->fn("tz_buffer_from_string", ptr, {ptr, i64, i32}),
          {this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value), enc},
          "buffer");
      this->lowering.refCounts().dropOperand(text);
      return made;
    }
    case Builtin::BufferAllocFill: {
      llvm::Value *size = this->lowering.exprLowerer().emitExpr(call->args[0]);
      Expr *fillExpr = call->args[1];
      Lowering::Operand fill = this->lowering.refCounts().emitOperand(
          fillExpr, AssignLowerer::cannotAssign(call->args[2]));
      llvm::Value *enc = this->encoding(call->args[2]);
      llvm::Value *made =
          this->lowering.builder.CreateCall(this->fn("tz_buffer_alloc", ptr, {i64, i32}),
                                            {size, this->lowering.builder.getInt32(1)}, "buffer");
      llvm::Value *zero = this->lowering.builder.getInt64(0),
                  *none = this->lowering.builder.getInt32(0);
      llvm::BasicBlock *fillBlock = this->lowering.functionFrame().newBlock("alloc.fill");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("alloc.end");
      this->lowering.builder.CreateCondBr(this->lowering.builder.CreateIsNotNull(made), fillBlock,
                                          endBlock);
      this->lowering.functionFrame().switchTo(fillBlock);
      if (fillExpr->type->isString()) {
        this->lowering.builder.CreateCall(
            this->fn("tz_buffer_fill_string", voidTy, {ptr, ptr, i64, i64, i64, i32, i32}),
            {made, this->lowering.stringLowerer().stringObj(fill.value),
             this->lowering.stringLowerer().stringMeta(fill.value), zero, zero, none, enc});
      } else if (fillExpr->type->isInteger()) {
        this->lowering.builder.CreateCall(
            this->fn("tz_buffer_fill_byte", voidTy, {ptr, i32, i64, i64, i32}),
            {made, this->lowering.builder.CreateTrunc(fill.value, i32), zero, zero, none});
      } else {
        this->lowering.builder.CreateCall(
            this->fn("tz_buffer_fill_bytes", voidTy, {ptr, ptr, i64, i64, i32}),
            {made, fill.value, zero, zero, none});
      }
      this->lowering.builder.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      this->lowering.refCounts().dropOperand(fill);
      return made;
    }
    case Builtin::BufferConcat: {
      Lowering::Operand list = this->lowering.refCounts().emitOperand(
          call->args[0], call->args.size() < 2 || AssignLowerer::cannotAssign(call->args[1]));
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(1, 1, values);
      llvm::Value *made =
          this->lowering.builder.CreateCall(this->fn("tz_buffer_concat", ptr, {ptr, i64, i32}),
                                            {list.value, values[0], has}, "buffer");
      this->lowering.refCounts().dropOperand(list);
      return made;
    }
    case Builtin::BufferByteLength: {
      Expr *source = call->args[0];
      if (source->type->isString()) {
        Lowering::Operand text = this->lowering.refCounts().emitOperand(
            source, AssignLowerer::cannotAssign(call->args[1]));
        llvm::Value *enc = this->encoding(call->args[1]);
        llvm::Value *length = this->lowering.builder.CreateCall(
            this->fn("tz_buffer_byte_length", i32, {ptr, i64, i32}),
            {this->lowering.stringLowerer().stringObj(text.value),
             this->lowering.stringLowerer().stringMeta(text.value), enc});
        this->lowering.refCounts().dropOperand(text);
        return this->lowering.builder.CreateSExt(length, i64);
      }
      Lowering::Operand bytes = this->lowering.refCounts().emitOperand(source, true);
      llvm::Value *length = this->lowering.bytesLowerer().bytesLength(bytes.value);
      this->lowering.refCounts().dropOperand(bytes);
      unsigned size = source->type->isTypedArray() ? source->type->elementSize() : 1;
      return this->lowering.builder.CreateMul(length, this->lowering.builder.getInt64(size));
    }
    case Builtin::BufferCompare:
    case Builtin::BufferEquals: {
      Expr *first = call->builtin() == Builtin::BufferCompare ? call->args[0] : member->object;
      Expr *second = call->builtin() == Builtin::BufferCompare ? call->args[1] : call->args[0];
      Lowering::Operand a =
          this->lowering.refCounts().emitOperand(first, AssignLowerer::cannotAssign(second));
      Lowering::Operand b = this->lowering.refCounts().emitOperand(second, true);
      llvm::Value *result =
          call->builtin() == Builtin::BufferCompare
              ? this->lowering.builder.CreateSExt(
                    this->lowering.builder.CreateCall(this->fn("tz_bytes_compare", i32, {ptr, ptr}),
                                                      {a.value, b.value}),
                    i64)
              : this->lowering.builder.CreateICmpNE(
                    this->lowering.builder.CreateCall(this->fn("tz_bytes_equal", i32, {ptr, ptr}),
                                                      {a.value, b.value}),
                    this->lowering.builder.getInt32(0));
      this->lowering.refCounts().dropOperand(b);
      this->lowering.refCounts().dropOperand(a);
      return result;
    }
    case Builtin::BufferCompareTo: {
      Lowering::Operand self = this->receiver(0);
      Lowering::Operand target = this->lowering.refCounts().emitOperand(
          call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                      AssignLowerer::cannotAssign));
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(1, 4, values);
      llvm::Value *result = this->lowering.builder.CreateCall(
          this->fn("tz_buffer_compare_range", i32, {ptr, ptr, i64, i64, i64, i64, i32}),
          {self.value, target.value, values[0], values[1], values[2], values[3], has});
      this->lowering.refCounts().dropOperand(target);
      this->lowering.refCounts().dropOperand(self);
      return this->lowering.builder.CreateSExt(result, i64);
    }
    case Builtin::BufferIsBuffer: {
      const Type *t = call->args[0]->type;
      if (!t->nonOptional()->isTypedArray() || t->nonOptional()->bytes() != Bytes::Uint8 ||
          t->isOptional()) {
        this->lowering.stmtLowerer().emitDiscarded(call->args[0]);
        return this->lowering.builder.getInt1(t->isBuffer());
      }
      Lowering::Operand bytes = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *is = this->lowering.builder.CreateCall(
          this->fn("tz_bytes_is_buffer", i32, {ptr}), {bytes.value});
      this->lowering.refCounts().dropOperand(bytes);
      return this->lowering.builder.CreateICmpNE(is, this->lowering.builder.getInt32(0));
    }
    case Builtin::BufferIsEncoding: {
      Lowering::Operand name = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *found = this->lowering.builder.CreateCall(
          this->fn("tz_buffer_encoding", i32, {ptr, i64}),
          {this->lowering.stringLowerer().stringObj(name.value),
           this->lowering.stringLowerer().stringMeta(name.value)});
      this->lowering.refCounts().dropOperand(name);
      return this->lowering.builder.CreateICmpSGE(found, this->lowering.builder.getInt32(0));
    }
    case Builtin::BufferToString: {
      Lowering::Operand self = this->receiver(0);
      llvm::Value *enc = this->encoding(call->args[0]);
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(1, 2, values);
      llvm::Value *text = this->lowering.stringLowerer().callReturningString(
          this->fn("tz_buffer_to_string", voidTy, {ptr, ptr, i32, i64, i64, i32}),
          {self.value, enc, values[0], values[1], has});
      this->lowering.refCounts().dropOperand(self);
      return text;
    }
    case Builtin::BufferWrite: {
      Lowering::Operand self = this->receiver(0);
      Lowering::Operand text = this->lowering.refCounts().emitOperand(
          call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                      AssignLowerer::cannotAssign));
      llvm::Value *enc = this->encoding(call->args[1]);
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(2, 2, values);
      llvm::Value *written = this->lowering.builder.CreateCall(
          this->fn("tz_buffer_write_string", i32, {ptr, ptr, i64, i64, i64, i32, i32}),
          {self.value, this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value), values[0], values[1], has, enc});
      this->lowering.refCounts().dropOperand(text);
      this->lowering.refCounts().dropOperand(self);
      return this->lowering.builder.CreateSExt(written, i64);
    }
    case Builtin::BufferIndexOf:
    case Builtin::BufferFill: {
      bool fill = call->builtin() == Builtin::BufferFill;
      size_t valueAt = fill ? 0 : 1;
      Expr *valueExpr = call->args[valueAt];
      Lowering::Operand self = this->receiver(valueAt);
      Lowering::Operand value = this->lowering.refCounts().emitOperand(
          valueExpr, llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(valueAt + 1),
                                  AssignLowerer::cannotAssign));
      llvm::Value *enc = this->encoding(call->args[valueAt + 1]);
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(valueAt + 2, fill ? 2 : 1, values);
      llvm::Value *result = nullptr;
      if (fill) {
        if (valueExpr->type->isString()) {
          this->lowering.builder.CreateCall(
              this->fn("tz_buffer_fill_string", voidTy, {ptr, ptr, i64, i64, i64, i32, i32}),
              {self.value, this->lowering.stringLowerer().stringObj(value.value),
               this->lowering.stringLowerer().stringMeta(value.value), values[0], values[1], has,
               enc});
        } else if (valueExpr->type->isInteger()) {
          this->lowering.builder.CreateCall(
              this->fn("tz_buffer_fill_byte", voidTy, {ptr, i32, i64, i64, i32}),
              {self.value, this->lowering.builder.CreateTrunc(value.value, i32), values[0],
               values[1], has});
        } else {
          this->lowering.builder.CreateCall(
              this->fn("tz_buffer_fill_bytes", voidTy, {ptr, ptr, i64, i64, i32}),
              {self.value, value.value, values[0], values[1], has});
        }
        this->lowering.refCounts().dropOperand(value);
        return this->returnSelf(self);
      }
      llvm::Value *mode =
          this->lowering.builder.getInt32(uint32_t(BytesCallSite::constantOf(call->args[0])));
      if (valueExpr->type->isString()) {
        result = this->lowering.builder.CreateCall(
            this->fn("tz_buffer_index_of_string", i32, {ptr, ptr, i64, i64, i32, i32, i32}),
            {self.value, this->lowering.stringLowerer().stringObj(value.value),
             this->lowering.stringLowerer().stringMeta(value.value), values[0], has, enc, mode});
      } else if (valueExpr->type->isInteger()) {
        result = this->lowering.builder.CreateCall(
            this->fn("tz_buffer_index_of_byte", i32, {ptr, i32, i64, i32, i32}),
            {self.value, this->lowering.builder.CreateTrunc(value.value, i32), values[0], has,
             mode});
      } else {
        result = this->lowering.builder.CreateCall(
            this->fn("tz_buffer_index_of_bytes", i32, {ptr, ptr, i64, i32, i32}),
            {self.value, value.value, values[0], has, mode});
      }
      this->lowering.refCounts().dropOperand(value);
      this->lowering.refCounts().dropOperand(self);
      return BytesCallSite::constantOf(call->args[0]) == 2
                 ? this->lowering.builder.CreateICmpNE(result, this->lowering.builder.getInt32(0))
                 : this->lowering.builder.CreateSExt(result, i64);
    }
    case Builtin::BufferCopy: {
      Lowering::Operand self = this->receiver(0);
      Lowering::Operand target = this->lowering.refCounts().emitOperand(
          call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                      AssignLowerer::cannotAssign));
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(1, 3, values);
      llvm::Value *copied = this->lowering.builder.CreateCall(
          this->fn("tz_buffer_copy", i32, {ptr, ptr, i64, i64, i64, i32}),
          {self.value, target.value, values[0], values[1], values[2], has});
      this->lowering.refCounts().dropOperand(target);
      this->lowering.refCounts().dropOperand(self);
      return this->lowering.builder.CreateSExt(copied, i64);
    }
    case Builtin::BufferSwap: {
      Lowering::Operand self = this->receiver(0);
      this->lowering.builder.CreateCall(
          this->fn("tz_buffer_swap", voidTy, {ptr, i32}),
          {self.value,
           this->lowering.builder.getInt32(uint32_t(BytesCallSite::constantOf(call->args[0])))});
      return this->returnSelf(self);
    }
    case Builtin::BufferToJSON: {
      Lowering::Operand self = this->receiver(0);
      llvm::Value *object = this->lowering.objectLayouts().allocObject(type, {0, 1});
      this->lowering.builder.CreateStore(
          this->lowering.stringLowerer().stringLiteral(u"Buffer"),
          this->lowering.objectLayouts().fieldAddress(object, type, "type"));
      llvm::Value *data = this->lowering.builder.CreateCall(
          this->fn("tz_bytes_to_array", ptr, {ptr, ptr}),
          {self.value, this->lowering.typeDescriptors().typeDescriptor(Type::intTy())});
      this->lowering.builder.CreateStore(
          data, this->lowering.objectLayouts().fieldAddress(object, type, "data"));
      this->lowering.refCounts().dropOperand(self);
      return object;
    }
    default:
      llvm_unreachable("not a call of BufferCalls");
  }
}

}
