#include "compiler/library/bytes/buffer_calls/buffer_calls.h"
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

llvm::Value *BufferCalls::emit() {
  switch (call->builtin) {
  case Builtin::BufferFromString: {
    Lowering::Operand text = lowering_.refCounts().emitOperand(
        call->args[0], AssignLowerer::cannotAssign(call->args[1]));
    lowering_.throwLowerer().holdOperand(text,
                                         throws || lowering_.mayThrow().exprThrows(call->args[1]));
    llvm::Value *enc = encoding(call->args[1]);
    lowering_.throwLowerer().takeOperand(text);
    llvm::Value *made =
        lowering_.builder_.CreateCall(fn("tz_buffer_from_string", ptr, {ptr, i64, i32}),
                                      {lowering_.stringLowerer().stringObj(text.value),
                                       lowering_.stringLowerer().stringMeta(text.value), enc},
                                      "buffer");
    lowering_.refCounts().dropOperand(text);
    return made;
  }
  case Builtin::BufferAllocFill: {
    llvm::Value *size = lowering_.exprLowerer().emitExpr(call->args[0]);
    Expr *fillExpr = call->args[1];
    Lowering::Operand fill =
        lowering_.refCounts().emitOperand(fillExpr, AssignLowerer::cannotAssign(call->args[2]));
    lowering_.throwLowerer().holdOperand(fill,
                                         throws || lowering_.mayThrow().exprThrows(call->args[2]));
    llvm::Value *enc = encoding(call->args[2]);
    lowering_.throwLowerer().takeOperand(fill);
    llvm::Value *made = lowering_.builder_.CreateCall(
        fn("tz_buffer_alloc", ptr, {i64, i32}), {size, lowering_.builder_.getInt32(1)}, "buffer");
    llvm::Value *zero = lowering_.builder_.getInt64(0), *none = lowering_.builder_.getInt32(0);
    llvm::BasicBlock *fillBlock = lowering_.functionFrame().newBlock("alloc.fill");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("alloc.end");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(made), fillBlock, endBlock);
    lowering_.functionFrame().switchTo(fillBlock);
    if (fillExpr->type->isString()) {
      lowering_.builder_.CreateCall(
          fn("tz_buffer_fill_string", voidTy, {ptr, ptr, i64, i64, i64, i32, i32}),
          {made, lowering_.stringLowerer().stringObj(fill.value),
           lowering_.stringLowerer().stringMeta(fill.value), zero, zero, none, enc});
    } else if (fillExpr->type->isInt()) {
      lowering_.builder_.CreateCall(fn("tz_buffer_fill_byte", voidTy, {ptr, i32, i64, i64, i32}),
                                    {made, fill.value, zero, zero, none});
    } else {
      lowering_.builder_.CreateCall(fn("tz_buffer_fill_bytes", voidTy, {ptr, ptr, i64, i64, i32}),
                                    {made, fill.value, zero, zero, none});
    }
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    lowering_.refCounts().dropOperand(fill);
    Lowering::Held held = lowering_.throwLowerer().hold(made, type, true);
    lowering_.throwLowerer().emitThrowCheck();
    return lowering_.throwLowerer().take(held);
  }
  case Builtin::BufferConcat: {
    Lowering::Operand list = lowering_.refCounts().emitOperand(
        call->args[0], call->args.size() < 2 || AssignLowerer::cannotAssign(call->args[1]));
    lowering_.throwLowerer().holdOperand(list, call->args.size() > 1 &&
                                                   lowering_.mayThrow().exprThrows(call->args[1]));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 1, values);
    lowering_.throwLowerer().takeOperand(list);
    llvm::Value *made = lowering_.builder_.CreateCall(fn("tz_buffer_concat", ptr, {ptr, i64, i32}),
                                                      {list.value, values[0], has}, "buffer");
    lowering_.refCounts().dropOperand(list);
    lowering_.throwLowerer().emitThrowCheck();
    return made;
  }
  case Builtin::BufferByteLength: {
    Expr *source = call->args[0];
    if (source->type->isString()) {
      Lowering::Operand text =
          lowering_.refCounts().emitOperand(source, AssignLowerer::cannotAssign(call->args[1]));
      lowering_.throwLowerer().holdOperand(
          text, throws || lowering_.mayThrow().exprThrows(call->args[1]));
      llvm::Value *enc = encoding(call->args[1]);
      lowering_.throwLowerer().takeOperand(text);
      llvm::Value *length =
          lowering_.builder_.CreateCall(fn("tz_buffer_byte_length", i32, {ptr, i64, i32}),
                                        {lowering_.stringLowerer().stringObj(text.value),
                                         lowering_.stringLowerer().stringMeta(text.value), enc});
      lowering_.refCounts().dropOperand(text);
      return length;
    }
    Lowering::Operand bytes = lowering_.refCounts().emitOperand(source, true);
    llvm::Value *length = lowering_.bytesLowerer().bytesLength(bytes.value);
    lowering_.refCounts().dropOperand(bytes);
    unsigned size = source->type->isTypedArray() ? source->type->elementSize() : 1;
    return lowering_.builder_.CreateTrunc(
        lowering_.builder_.CreateMul(length, lowering_.builder_.getInt64(size)), i32);
  }
  case Builtin::BufferCompare:
  case Builtin::BufferEquals: {
    Expr *first = call->builtin == Builtin::BufferCompare ? call->args[0] : member->object;
    Expr *second = call->builtin == Builtin::BufferCompare ? call->args[1] : call->args[0];
    Lowering::Operand a =
        lowering_.refCounts().emitOperand(first, AssignLowerer::cannotAssign(second));
    lowering_.throwLowerer().holdOperand(a, lowering_.mayThrow().exprThrows(second));
    Lowering::Operand b = lowering_.refCounts().emitOperand(second, true);
    lowering_.throwLowerer().takeOperand(a);
    llvm::Value *result = call->builtin == Builtin::BufferCompare
                              ? lowering_.builder_.CreateCall(
                                    fn("tz_bytes_compare", i32, {ptr, ptr}), {a.value, b.value})
                              : lowering_.builder_.CreateICmpNE(
                                    lowering_.builder_.CreateCall(
                                        fn("tz_bytes_equal", i32, {ptr, ptr}), {a.value, b.value}),
                                    lowering_.builder_.getInt32(0));
    lowering_.refCounts().dropOperand(b);
    lowering_.refCounts().dropOperand(a);
    return result;
  }
  case Builtin::BufferCompareTo: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand target = lowering_.refCounts().emitOperand(
        call->args[0],
        llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), AssignLowerer::cannotAssign));
    lowering_.throwLowerer().holdOperand(
        target, llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                             [&](Expr *e) { return lowering_.mayThrow().exprThrows(e); }));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 4, values);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.throwLowerer().takeOperand(target);
    llvm::Value *result = lowering_.builder_.CreateCall(
        fn("tz_buffer_compare_range", i32, {ptr, ptr, i64, i64, i64, i64, i32}),
        {self.value, target.value, values[0], values[1], values[2], values[3], has});
    lowering_.refCounts().dropOperand(target);
    lowering_.refCounts().dropOperand(self);
    lowering_.throwLowerer().emitThrowCheck();
    return result;
  }
  case Builtin::BufferIsBuffer: {
    const Type *t = call->args[0]->type;
    if (!t->nonOptional()->isTypedArray() || t->nonOptional()->bytes() != Bytes::Uint8 ||
        t->isOptional()) {
      lowering_.stmtLowerer().emitDiscarded(call->args[0]);
      return lowering_.builder_.getInt1(t->isBuffer());
    }
    Lowering::Operand bytes = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *is =
        lowering_.builder_.CreateCall(fn("tz_bytes_is_buffer", i32, {ptr}), {bytes.value});
    lowering_.refCounts().dropOperand(bytes);
    return lowering_.builder_.CreateICmpNE(is, lowering_.builder_.getInt32(0));
  }
  case Builtin::BufferIsEncoding: {
    Lowering::Operand name = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *found =
        lowering_.builder_.CreateCall(fn("tz_buffer_encoding", i32, {ptr, i64}),
                                      {lowering_.stringLowerer().stringObj(name.value),
                                       lowering_.stringLowerer().stringMeta(name.value)});
    lowering_.refCounts().dropOperand(name);
    return lowering_.builder_.CreateICmpSGE(found, lowering_.builder_.getInt32(0));
  }
  case Builtin::BufferToString: {
    Lowering::Operand self = receiver(0);
    llvm::Value *enc = encoding(call->args[0]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 2, values);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *text = lowering_.stringLowerer().callReturningString(
        fn("tz_buffer_to_string", voidTy, {ptr, ptr, i32, i64, i64, i32}),
        {self.value, enc, values[0], values[1], has});
    lowering_.refCounts().dropOperand(self);
    return text;
  }
  case Builtin::BufferWrite: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand text = lowering_.refCounts().emitOperand(
        call->args[0],
        llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), AssignLowerer::cannotAssign));
    lowering_.throwLowerer().holdOperand(
        text, throws || llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                     [&](Expr *e) { return lowering_.mayThrow().exprThrows(e); }));
    llvm::Value *enc = encoding(call->args[1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(2, 2, values);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.throwLowerer().takeOperand(text);
    llvm::Value *written = lowering_.builder_.CreateCall(
        fn("tz_buffer_write_string", i32, {ptr, ptr, i64, i64, i64, i32, i32}),
        {self.value, lowering_.stringLowerer().stringObj(text.value),
         lowering_.stringLowerer().stringMeta(text.value), values[0], values[1], has, enc});
    lowering_.refCounts().dropOperand(text);
    lowering_.refCounts().dropOperand(self);
    lowering_.throwLowerer().emitThrowCheck();
    return written;
  }
  case Builtin::BufferIndexOf:
  case Builtin::BufferFill: {
    bool fill = call->builtin == Builtin::BufferFill;
    size_t valueAt = fill ? 0 : 1;
    Expr *valueExpr = call->args[valueAt];
    Lowering::Operand self = receiver(valueAt);
    Lowering::Operand value = lowering_.refCounts().emitOperand(
        valueExpr, llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(valueAt + 1),
                                AssignLowerer::cannotAssign));
    lowering_.throwLowerer().holdOperand(
        value, throws || llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(valueAt + 1),
                                      [&](Expr *e) { return lowering_.mayThrow().exprThrows(e); }));
    llvm::Value *enc = encoding(call->args[valueAt + 1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(valueAt + 2, fill ? 2 : 1, values);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.throwLowerer().takeOperand(value);
    llvm::Value *result = nullptr;
    if (fill) {
      if (valueExpr->type->isString()) {
        lowering_.builder_.CreateCall(
            fn("tz_buffer_fill_string", voidTy, {ptr, ptr, i64, i64, i64, i32, i32}),
            {self.value, lowering_.stringLowerer().stringObj(value.value),
             lowering_.stringLowerer().stringMeta(value.value), values[0], values[1], has, enc});
      } else if (valueExpr->type->isInt()) {
        lowering_.builder_.CreateCall(fn("tz_buffer_fill_byte", voidTy, {ptr, i32, i64, i64, i32}),
                                      {self.value, value.value, values[0], values[1], has});
      } else {
        lowering_.builder_.CreateCall(fn("tz_buffer_fill_bytes", voidTy, {ptr, ptr, i64, i64, i32}),
                                      {self.value, value.value, values[0], values[1], has});
      }
      lowering_.refCounts().dropOperand(value);
      Lowering::Held held = lowering_.throwLowerer().hold(returnSelf(self), self.type, true);
      lowering_.throwLowerer().emitThrowCheck();
      return lowering_.throwLowerer().take(held);
    }
    llvm::Value *mode = lowering_.builder_.getInt32(uint32_t(constantOf(call->args[0])));
    if (valueExpr->type->isString()) {
      result = lowering_.builder_.CreateCall(
          fn("tz_buffer_index_of_string", i32, {ptr, ptr, i64, i64, i32, i32, i32}),
          {self.value, lowering_.stringLowerer().stringObj(value.value),
           lowering_.stringLowerer().stringMeta(value.value), values[0], has, enc, mode});
    } else if (valueExpr->type->isInt()) {
      result = lowering_.builder_.CreateCall(
          fn("tz_buffer_index_of_byte", i32, {ptr, i32, i64, i32, i32}),
          {self.value, value.value, values[0], has, mode});
    } else {
      result = lowering_.builder_.CreateCall(
          fn("tz_buffer_index_of_bytes", i32, {ptr, ptr, i64, i32, i32}),
          {self.value, value.value, values[0], has, mode});
    }
    lowering_.refCounts().dropOperand(value);
    lowering_.refCounts().dropOperand(self);
    return constantOf(call->args[0]) == 2
               ? lowering_.builder_.CreateICmpNE(result, lowering_.builder_.getInt32(0))
               : result;
  }
  case Builtin::BufferCopy: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand target = lowering_.refCounts().emitOperand(
        call->args[0],
        llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), AssignLowerer::cannotAssign));
    lowering_.throwLowerer().holdOperand(
        target, llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                             [&](Expr *e) { return lowering_.mayThrow().exprThrows(e); }));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 3, values);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.throwLowerer().takeOperand(target);
    llvm::Value *copied = lowering_.builder_.CreateCall(
        fn("tz_buffer_copy", i32, {ptr, ptr, i64, i64, i64, i32}),
        {self.value, target.value, values[0], values[1], values[2], has});
    lowering_.refCounts().dropOperand(target);
    lowering_.refCounts().dropOperand(self);
    lowering_.throwLowerer().emitThrowCheck();
    return copied;
  }
  case Builtin::BufferSwap: {
    Lowering::Operand self = receiver(0);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.builder_.CreateCall(
        fn("tz_buffer_swap", voidTy, {ptr, i32}),
        {self.value, lowering_.builder_.getInt32(uint32_t(constantOf(call->args[0])))});
    Lowering::Held held = lowering_.throwLowerer().hold(returnSelf(self), self.type, true);
    lowering_.throwLowerer().emitThrowCheck();
    return lowering_.throwLowerer().take(held);
  }
  case Builtin::BufferToJSON: {
    Lowering::Operand self = receiver(0);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *object = lowering_.objectLayouts().allocObject(type, {0, 1});
    lowering_.builder_.CreateStore(lowering_.stringLowerer().stringLiteral(u"Buffer"),
                                   lowering_.objectLayouts().fieldAddress(object, type, "type"));
    llvm::Value *data = lowering_.builder_.CreateCall(
        fn("tz_bytes_to_array", ptr, {ptr, ptr}),
        {self.value, lowering_.typeDescriptors().typeDescriptor(Type::intTy())});
    lowering_.builder_.CreateStore(data,
                                   lowering_.objectLayouts().fieldAddress(object, type, "data"));
    lowering_.refCounts().dropOperand(self);
    return object;
  }
  default:
    llvm_unreachable("not a call of BufferCalls");
  }
}

}
