#include "compiler/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/library/bytes/buffer_calls/buffer_calls.h"
#include "compiler/library/bytes/bytes_call_site/bytes_call_site.h"
#include "compiler/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/bytes/bytes_make_calls/bytes_make_calls.h"
#include "compiler/library/bytes/number_access_calls/number_access_calls.h"
#include "compiler/library/bytes/text_codec_calls/text_codec_calls.h"
#include "compiler/library/bytes/typed_array_calls/typed_array_calls.h"
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

bool bytesMayThrow(const CallExpr *call) {
  switch (call->builtin) {
  case Builtin::BufferAllocFill:
  case Builtin::BufferFill:
    return true;
  case Builtin::TextDecoderNew:
    return constantOf(call->args[0]) == 0;
  case Builtin::BufferFromString:
    return !isConstant(call->args[1]);
  case Builtin::BufferByteLength:
    return call->args.size() > 1 && !isConstant(call->args[1]);
  case Builtin::BufferToString:
    return !isConstant(call->args[0]);
  case Builtin::BufferIndexOf:
    return !isConstant(call->args[2]);
  default:
    return false;
  }
}

BytesCallLowerer::BytesCallLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *BytesCallLowerer::emitBytesCall(CallExpr *call) {
  switch (call->builtin) {
  case Builtin::BytesNew:
  case Builtin::ArrayBufferNew:
  case Builtin::BytesFromArray:
  case Builtin::BytesFromBytes:
  case Builtin::BytesOf:
  case Builtin::BytesView:
  case Builtin::DataViewNew:
    return BytesMakeCalls(lowering_, call).emit();
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
    return TypedArrayCalls(lowering_, call).emit();
  case Builtin::DataViewGet:
  case Builtin::DataViewSet:
  case Builtin::BufferRead:
  case Builtin::BufferWriteNumber:
  case Builtin::BufferReadVar:
  case Builtin::BufferWriteVar:
    return NumberAccessCalls(lowering_, call).emit();
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
    return BufferCalls(lowering_, call).emit();
  case Builtin::TextEncoderNew:
  case Builtin::TextEncode:
  case Builtin::TextEncodeInto:
  case Builtin::TextDecoderNew:
  case Builtin::TextDecode:
  case Builtin::TextDecoderInfo:
    return TextCodecCalls(lowering_, call).emit();
  case Builtin::EmbedBytes:
    return emitEmbedBytes(call);
  default:
    llvm_unreachable("not a built-in of binary data");
  }
}

llvm::Value *BytesCallLowerer::emitEmbedBytes(CallExpr *call) {
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *i64 = lowering_.builder_.getInt64Ty(),
             *ptr = lowering_.builder_.getPtrTy();
  const std::u16string &path = cast<StringLitExpr>(call->args[1])->value;
  llvm::Constant *&global = embeddedBytes_[path];
  if (!global) {
    const std::u16string &units = cast<StringLitExpr>(call->args[0])->value;
    std::vector<uint8_t> bytes;
    bytes.reserve(units.size());
    for (char16_t unit : units) {
      bytes.push_back(uint8_t(unit));
    }
    llvm::Constant *init = llvm::ConstantDataArray::get(lowering_.ctx_, bytes);
    auto *data =
        new llvm::GlobalVariable(*lowering_.module_, init->getType(), false,
                                 llvm::GlobalValue::PrivateLinkage, init, "tz.embedded.data");
    data->setAlignment(llvm::Align(16));
    auto *layoutsTy = llvm::ArrayType::get(lowering_.layoutDescTy_, 16);
    auto *layouts = cast<llvm::GlobalVariable>(
        lowering_.module_->getOrInsertGlobal("tz_bytes_layouts", layoutsTy));
    auto layout = [&](Bytes kind) {
      return llvm::ConstantExpr::getInBoundsGetElementPtr(
          layoutsTy, layouts,
          llvm::ArrayRef<llvm::Constant *>{lowering_.builder_.getInt32(0),
                                           lowering_.builder_.getInt32(uint32_t(kind))});
    };
    llvm::Constant *count = lowering_.builder_.getInt64(uint64_t(1) << 62);
    llvm::Constant *length = lowering_.builder_.getInt32(uint32_t(bytes.size()));
    auto *abufTy = llvm::StructType::get(lowering_.ctx_, {i64, ptr, ptr, i32, i32});
    auto *abuf = new llvm::GlobalVariable(
        *lowering_.module_, abufTy, false, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(abufTy, {count, layout(Bytes::ArrayBuffer), data, length,
                                           lowering_.builder_.getInt32(1)}),
        "tz.embedded.buffer");
    if (!lowering_.bytesTy_) {
      lowering_.bytesTy_ = bytesStruct(lowering_.ctx_);
    }
    global = new llvm::GlobalVariable(
        *lowering_.module_, lowering_.bytesTy_, false, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(lowering_.bytesTy_, {count, layout(Bytes::Uint8), data, length,
                                                       lowering_.builder_.getInt32(0), abuf}),
        "tz.embedded");
  }

  return global;
}

}
