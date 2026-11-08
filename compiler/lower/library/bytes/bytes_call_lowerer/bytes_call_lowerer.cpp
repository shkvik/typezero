#include "compiler/lower/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/bytes/buffer_calls/buffer_calls.h"
#include "compiler/lower/library/bytes/bytes_call_site/bytes_call_site.h"
#include "compiler/lower/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/lower/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/lower/library/bytes/bytes_make_calls/bytes_make_calls.h"
#include "compiler/lower/library/bytes/number_access_calls/number_access_calls.h"
#include "compiler/lower/library/bytes/text_codec_calls/text_codec_calls.h"
#include "compiler/lower/library/bytes/typed_array_calls/typed_array_calls.h"
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

using llvm::cast;

namespace tz {

BytesCallLowerer::BytesCallLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *BytesCallLowerer::emitBytesCall(CallExpr *call) {
  switch (call->builtin()) {
    case Builtin::BytesNew:
    case Builtin::ArrayBufferNew:
    case Builtin::BytesFromArray:
    case Builtin::BytesFromBytes:
    case Builtin::BytesOf:
    case Builtin::BytesView:
    case Builtin::DataViewNew:
      return BytesMakeCalls(this->lowering, call).emit();
    case Builtin::BytesByteLength:
    case Builtin::BytesByteOffset:
    case Builtin::BytesBuffer:
    case Builtin::BytesAt:
    case Builtin::BytesIndexOf:
    case Builtin::BytesJoin:
    case Builtin::BytesReverse:
    case Builtin::BytesSort:
    case Builtin::BytesFill:
    case Builtin::BytesSet:
    case Builtin::BytesSubarray:
    case Builtin::BytesSlice:
    case Builtin::ArrayBufferSlice:
    case Builtin::BytesCopyWithin:
    case Builtin::BytesToReversed:
    case Builtin::BytesToSorted:
    case Builtin::BytesWith:
    case Builtin::ArrayBufferIsView:
      return TypedArrayCalls(this->lowering, call).emit();
    case Builtin::DataViewGet:
    case Builtin::DataViewSet:
    case Builtin::BufferRead:
    case Builtin::BufferWriteNumber:
    case Builtin::BufferReadVar:
    case Builtin::BufferWriteVar:
      return NumberAccessCalls(this->lowering, call).emit();
    case Builtin::BufferFromString:
    case Builtin::BufferAllocFill:
    case Builtin::BufferConcat:
    case Builtin::BufferByteLength:
    case Builtin::BufferCompare:
    case Builtin::BufferEquals:
    case Builtin::BufferCompareTo:
    case Builtin::BufferIsBuffer:
    case Builtin::BufferIsEncoding:
    case Builtin::BufferToString:
    case Builtin::BufferWrite:
    case Builtin::BufferIndexOf:
    case Builtin::BufferFill:
    case Builtin::BufferCopy:
    case Builtin::BufferSwap:
    case Builtin::BufferToJSON:
      return BufferCalls(this->lowering, call).emit();
    case Builtin::TextEncoderNew:
    case Builtin::TextEncode:
    case Builtin::TextEncodeInto:
    case Builtin::TextDecoderNew:
    case Builtin::TextDecoderParse:
    case Builtin::TextDecode:
    case Builtin::TextDecoderInfo:
      return TextCodecCalls(this->lowering, call).emit();
    case Builtin::EmbedBytes:
      return this->emitEmbedBytes(call);
    default:
      llvm_unreachable("not a built-in of binary data");
  }
}

llvm::Value *BytesCallLowerer::emitEmbedBytes(CallExpr *call) {
  llvm::Type *i32 = this->lowering.builder.getInt32Ty(), *i64 = this->lowering.builder.getInt64Ty(),
             *ptr = this->lowering.builder.getPtrTy();
  const std::u16string &path = cast<StringLitExpr>(call->args[1])->value;
  llvm::Constant *&global = this->embeddedBytes[path];
  if (!global) {
    const std::u16string &units = cast<StringLitExpr>(call->args[0])->value;
    std::vector<uint8_t> bytes;
    bytes.reserve(units.size());
    for (char16_t unit : units) {
      bytes.push_back(uint8_t(unit));
    }
    llvm::Constant *init = llvm::ConstantDataArray::get(this->lowering.ctx, bytes);
    auto *data =
        new llvm::GlobalVariable(*this->lowering.module_, init->getType(), false,
                                 llvm::GlobalValue::PrivateLinkage, init, "tz.embedded.data");
    data->setAlignment(llvm::Align(16));
    auto *layoutsTy = llvm::ArrayType::get(this->lowering.layoutDescTy, 16);
    auto *layouts = cast<llvm::GlobalVariable>(
        this->lowering.module_->getOrInsertGlobal("tz_bytes_layouts", layoutsTy));
    auto layout = [&](Bytes kind) {
      return llvm::ConstantExpr::getInBoundsGetElementPtr(
          layoutsTy, layouts,
          llvm::ArrayRef<llvm::Constant *>{this->lowering.builder.getInt32(0),
                                           this->lowering.builder.getInt32(uint32_t(kind))});
    };
    llvm::Constant *count = this->lowering.builder.getInt64(uint64_t(1) << 62);
    llvm::Constant *length = this->lowering.builder.getInt32(uint32_t(bytes.size()));
    auto *abufTy = llvm::StructType::get(this->lowering.ctx, {i64, ptr, ptr, i32, i32});
    auto *abuf = new llvm::GlobalVariable(
        *this->lowering.module_, abufTy, false, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(abufTy, {count, layout(Bytes::ArrayBuffer), data, length,
                                           this->lowering.builder.getInt32(1)}),
        "tz.embedded.buffer");
    if (!this->lowering.bytesTy) {
      this->lowering.bytesTy = BytesLowerer::bytesStruct(this->lowering.ctx);
    }
    global = new llvm::GlobalVariable(
        *this->lowering.module_, this->lowering.bytesTy, false, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(
            this->lowering.bytesTy,
            {count, layout(Bytes::Uint8), data, length, this->lowering.builder.getInt32(0), abuf}),
        "tz.embedded");
  }

  return global;
}

}
