#include "compiler/library/bytes/number_access_calls/number_access_calls.h"
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

llvm::Value *NumberAccessCalls::emit() {
  switch (call->builtin) {
  case Builtin::DataViewGet:
  case Builtin::DataViewSet:
  case Builtin::BufferRead:
  case Builtin::BufferWriteNumber: {
    bool dataView = call->builtin == Builtin::DataViewGet || call->builtin == Builtin::DataViewSet;
    bool write =
        call->builtin == Builtin::DataViewSet || call->builtin == Builtin::BufferWriteNumber;
    const Type *kindType = Type::bytesType(Bytes(constantOf(call->args[0])));
    Bytes kind = kindType->bytes();
    unsigned size = kindType->elementSize();
    Lowering::Operand self = receiver(1);
    llvm::Value *offset = nullptr, *value = nullptr, *little = nullptr;
    if (dataView) {
      offset = lowering_.exprLowerer().emitExpr(call->args[1]);
      if (write) {
        value = lowering_.exprLowerer().emitExpr(call->args[2]);
      }
      size_t at = write ? 3 : 2;
      little = call->args.size() > at ? lowering_.exprLowerer().emitExpr(call->args[at])
                                      : lowering_.builder_.getFalse();
    } else {
      little = lowering_.builder_.getInt1(!cast<BoolLitExpr>(call->args[1])->value);
      size_t at = 2;
      if (write) {
        value = lowering_.exprLowerer().emitExpr(call->args[at++]);
      }
      offset = call->args.size() > at ? lowering_.exprLowerer().emitExpr(call->args[at])
                                      : lowering_.builder_.getInt64(0);
    }
    if (call->mayBeOutOfRange) {
      llvm::Value *length = lowering_.bytesLowerer().bytesLength(self.value);
      llvm::Value *ok = lowering_.builder_.CreateAnd(
          lowering_.builder_.CreateICmpULT(offset, length),
          lowering_.builder_.CreateICmpULE(lowering_.builder_.getInt64(size),
                                           lowering_.builder_.CreateSub(length, offset)));
      llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("bytes.out.of.range");
      llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("bytes.in.range");
      lowering_.builder_.CreateCondBr(ok, okBlock, badBlock,
                                      llvm::MDBuilder(lowering_.ctx_).createLikelyBranchWeights());
      lowering_.functionFrame().switchTo(badBlock);
      lowering_.builder_.CreateCall(fn("tz_bytes_out_of_range", voidTy, {i64, i64, i64}),
                                    {offset, lowering_.builder_.getInt64(size), length});
      lowering_.builder_.CreateUnreachable();
      lowering_.functionFrame().switchTo(okBlock);
    }
    llvm::Value *address = lowering_.builder_.CreateInBoundsGEP(
        lowering_.builder_.getInt8Ty(), lowering_.bytesLowerer().bytesData(self.value), offset);
    llvm::Type *bits = lowering_.builder_.getIntNTy(size * 8);
    auto swapped = [&](llvm::Value *raw) -> llvm::Value * {
      if (size == 1) {
        return raw;
      }
      llvm::Value *swap = lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, raw);
      if (auto *constant = dyn_cast<llvm::ConstantInt>(little)) {
        return constant->isOne() ? raw : swap;
      }

      return lowering_.builder_.CreateSelect(little, raw, swap);
    };
    llvm::Value *result;
    if (!write) {
      llvm::LoadInst *raw = lowering_.builder_.CreateAlignedLoad(bits, address, llvm::Align(1));
      raw->setMetadata(llvm::LLVMContext::MD_tbaa, lowering_.bytesLowerer().bytesTbaa(false));
      llvm::Value *v = swapped(raw);
      switch (kind) {
      case Bytes::Float32:
        result = lowering_.builder_.CreateFPExt(
            lowering_.builder_.CreateBitCast(v, lowering_.builder_.getFloatTy()), f64);
        break;
      case Bytes::Float64:
        result = lowering_.builder_.CreateBitCast(v, f64);
        break;
      case Bytes::Float16:
        result = lowering_.builder_.CreateCall(fn("tz_f16_to_f64", f64, {i32}),
                                               {lowering_.builder_.CreateZExt(v, i32)});
        break;
      case Bytes::Uint32:
        result = lowering_.builder_.CreateZExt(v, i64);
        break;
      case Bytes::Int32:
        result = v;
        break;
      default:
        result = isSignedKind(kind) ? lowering_.builder_.CreateSExt(v, i32)
                                    : lowering_.builder_.CreateZExt(v, i32);
        break;
      }
    } else {
      llvm::Value *v;
      switch (kind) {
      case Bytes::Float32:
        v = lowering_.builder_.CreateBitCast(
            lowering_.builder_.CreateFPTrunc(value, lowering_.builder_.getFloatTy()), bits);
        break;
      case Bytes::Float64:
        v = lowering_.builder_.CreateBitCast(value, bits);
        break;
      case Bytes::Float16:
        v = lowering_.builder_.CreateTrunc(
            lowering_.builder_.CreateCall(fn("tz_f16_from_f64", i32, {f64}), {value}), bits);
        break;
      default:
        v = value->getType() == bits ? value : lowering_.builder_.CreateTrunc(value, bits);
        break;
      }
      llvm::StoreInst *store =
          lowering_.builder_.CreateAlignedStore(swapped(v), address, llvm::Align(1));
      store->setMetadata(llvm::LLVMContext::MD_tbaa, lowering_.bytesLowerer().bytesTbaa(false));
      result =
          dataView
              ? nullptr
              : lowering_.builder_.CreateTrunc(
                    lowering_.builder_.CreateAdd(offset, lowering_.builder_.getInt64(size)), i32);
    }
    lowering_.refCounts().dropOperand(self);
    return result;
  }
  case Builtin::BufferReadVar:
  case Builtin::BufferWriteVar: {
    bool write = call->builtin == Builtin::BufferWriteVar;
    Lowering::Operand self = receiver(2);
    std::vector<llvm::Value *> values;
    for (size_t i = 2; i < call->args.size(); ++i) {
      values.push_back(lowering_.exprLowerer().emitExpr(call->args[i]));
    }
    uint32_t flags = uint32_t(cast<BoolLitExpr>(call->args[0])->value) |
                     uint32_t(cast<BoolLitExpr>(call->args[1])->value) << 1;
    llvm::Value *result;
    if (write) {
      result = lowering_.builder_.CreateCall(
          fn("tz_buffer_write_var", i64, {ptr, i64, i64, i64, i32}),
          {self.value, values[0], values[1], values[2], lowering_.builder_.getInt32(flags)});
      result = lowering_.builder_.CreateTrunc(result, i32);
    } else {
      result = lowering_.builder_.CreateCall(
          fn("tz_buffer_read_var", i64, {ptr, i64, i64, i32}),
          {self.value, values[0], values[1], lowering_.builder_.getInt32(flags)});
    }
    lowering_.refCounts().dropOperand(self);
    return result;
  }
  default:
    llvm_unreachable("not a call of NumberAccessCalls");
  }
}

}
