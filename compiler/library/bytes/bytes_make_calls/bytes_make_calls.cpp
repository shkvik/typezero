#include "compiler/library/bytes/bytes_make_calls/bytes_make_calls.h"
#include "compiler/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
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

llvm::Value *BytesMakeCalls::emit() {
  switch (call->builtin()) {
    case Builtin::BytesNew:
    case Builtin::ArrayBufferNew: {
      llvm::Value *length = this->lowering.exprLowerer().emitExpr(call->args[0]);
      llvm::Value *made;
      if (call->builtin() == Builtin::ArrayBufferNew) {
        made = this->lowering.builder_.CreateCall(this->fn("tz_abuf_new", ptr, {i64}), {length},
                                                  "buffer");
      } else if (type->isBuffer()) {
        made = this->lowering.builder_.CreateCall(
            this->fn("tz_buffer_alloc", ptr, {i64, i32}),
            {length, this->lowering.builder_.getInt32(call->args.size() < 2 ||
                                                      cast<BoolLitExpr>(call->args[1])->value)},
            "buffer");
      } else {
        made = this->lowering.builder_.CreateCall(this->fn("tz_bytes_new", ptr, {i32, i64}),
                                                  {this->kindOf(type), length}, "bytes");
      }
      return made;
    }
    case Builtin::BytesFromArray: {
      Expr *source = call->args[0];
      llvm::Value *made;
      if (source->type->isIterator()) {
        const Type *element = source->type->yielded();
        llvm::Value *values = this->lowering.builder_.CreateCall(
            this->lowering.rt_.arrNew,
            {this->lowering.typeDescriptors().typeDescriptor(element),
             this->lowering.builder_.getInt64(0), this->lowering.builder_.getInt64(0)},
            "values");
        this->lowering.iteratorLowerer().emitIteratorSpread(values, source, element);
        made = this->lowering.builder_.CreateCall(this->fn("tz_bytes_from_array", ptr, {i32, ptr}),
                                                  {this->kindOf(type), values}, "bytes");
        this->lowering.refCounts().releaseValue(values, Type::array(element));
        return made;
      }
      Lowering::Operand array = this->lowering.refCounts().emitOperand(source, true);
      made = this->lowering.builder_.CreateCall(this->fn("tz_bytes_from_array", ptr, {i32, ptr}),
                                                {this->kindOf(type), array.value}, "bytes");
      this->lowering.refCounts().dropOperand(array);
      return made;
    }
    case Builtin::BytesFromBytes: {
      Lowering::Operand source = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *made =
          this->lowering.builder_.CreateCall(this->fn("tz_bytes_from_bytes", ptr, {i32, ptr}),
                                             {this->kindOf(type), source.value}, "bytes");
      this->lowering.refCounts().dropOperand(source);
      return made;
    }
    case Builtin::BytesOf: {
      llvm::Value *made = this->lowering.builder_.CreateCall(
          this->fn("tz_bytes_new_unset", ptr, {i32, i64}),
          {this->kindOf(type), this->lowering.builder_.getInt64(call->args.size())}, "bytes");
      if (call->args.empty()) {
        return made;
      }
      for (size_t i = 0; i < call->args.size(); ++i) {
        this->lowering.bytesLowerer().storeElement(
            this->lowering.bytesLowerer().bytesData(made), type,
            this->lowering.builder_.getInt64(i),
            this->lowering.exprLowerer().emitExpr(call->args[i]));
      }
      return made;
    }
    case Builtin::BytesView:
    case Builtin::DataViewNew: {
      Lowering::Operand buffer = this->lowering.refCounts().emitOperand(
          call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                      AssignLowerer::cannotAssign));
      std::vector<llvm::Value *> values;
      llvm::Value *has = this->positions(1, 2, values);
      llvm::Value *made;
      if (call->builtin() == Builtin::DataViewNew) {
        made = this->lowering.builder_.CreateCall(
            this->fn("tz_dataview_new", ptr, {ptr, i64, i64, i32}),
            {buffer.value, values[0], values[1], has}, "view");
      } else if (type->isBuffer()) {
        made = this->lowering.builder_.CreateCall(
            this->fn("tz_buffer_view", ptr, {ptr, i64, i64, i32}),
            {buffer.value, values[0], values[1], has}, "view");
      } else {
        made = this->lowering.builder_.CreateCall(
            this->fn("tz_bytes_view", ptr, {i32, ptr, i64, i64, i32}),
            {this->kindOf(type), buffer.value, values[0], values[1], has}, "view");
      }
      this->lowering.refCounts().dropOperand(buffer);
      return made;
    }
    default:
      llvm_unreachable("not a call of BytesMakeCalls");
  }
}

}
