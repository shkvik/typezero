#include "compiler/lower/library/bytes/number_access_calls/number_access_calls.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/lower/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
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

namespace {

constexpr double kFloatTop = 3.4028234663852886e38;

}

llvm::Value *NumberAccessCalls::emit() {
  switch (call->builtin()) {
    case Builtin::DataViewGet:
    case Builtin::DataViewSet:
    case Builtin::BufferRead:
    case Builtin::BufferWriteNumber: {
      bool dataView =
          call->builtin() == Builtin::DataViewGet || call->builtin() == Builtin::DataViewSet;
      bool write =
          call->builtin() == Builtin::DataViewSet || call->builtin() == Builtin::BufferWriteNumber;
      const Type *kindType = Type::bytesType(Bytes(BytesCallSite::constantOf(call->args[0])));
      Bytes kind = kindType->bytes();
      unsigned size = kindType->elementSize();
      Lowering::Operand self = this->receiver(1);
      llvm::Value *offset = nullptr, *value = nullptr, *little = nullptr;
      if (dataView) {
        offset = this->lowering.exprLowerer().emitExpr(call->args[1]);
        if (write) {
          value = this->lowering.exprLowerer().emitExpr(call->args[2]);
        }
        size_t at = write ? 3 : 2;
        little = call->args.size() > at ? this->lowering.exprLowerer().emitExpr(call->args[at])
                                        : this->lowering.builder.getFalse();
      } else {
        little = this->lowering.builder.getInt1(!cast<BoolLitExpr>(call->args[1])->value);
        size_t at = 2;
        if (write) {
          value = this->lowering.exprLowerer().emitExpr(call->args[at++]);
        }
        offset = call->args.size() > at ? this->lowering.exprLowerer().emitExpr(call->args[at])
                                        : this->lowering.builder.getInt64(0);
      }
      if (call->mayBeOutOfRange) {
        llvm::Value *length = this->lowering.bytesLowerer().bytesLength(self.value);
        llvm::Value *ok = this->lowering.builder.CreateAnd(
            this->lowering.builder.CreateICmpULT(offset, length),
            this->lowering.builder.CreateICmpULE(this->lowering.builder.getInt64(size),
                                                 this->lowering.builder.CreateSub(length, offset)));
        llvm::BasicBlock *badBlock = this->lowering.functionFrame().newBlock("bytes.out.of.range");
        llvm::BasicBlock *okBlock = this->lowering.functionFrame().newBlock("bytes.in.range");
        this->lowering.builder.CreateCondBr(
            ok, okBlock, badBlock, llvm::MDBuilder(this->lowering.ctx).createLikelyBranchWeights());
        this->lowering.functionFrame().switchTo(badBlock);
        this->lowering.builder.CreateCall(
            this->fn("tz_bytes_out_of_range", voidTy, {i64, i64, i64}),
            {offset, this->lowering.builder.getInt64(size), length});
        this->lowering.builder.CreateUnreachable();
        this->lowering.functionFrame().switchTo(okBlock);
      }
      llvm::Value *address = this->lowering.builder.CreateInBoundsGEP(
          this->lowering.builder.getInt8Ty(), this->lowering.bytesLowerer().bytesData(self.value),
          offset);
      llvm::Type *bits = this->lowering.builder.getIntNTy(size * 8);
      auto swapped = [&](llvm::Value *raw) -> llvm::Value * {
        if (size == 1) {
          return raw;
        }
        llvm::Value *swap =
            this->lowering.builder.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, raw);
        if (auto *constant = dyn_cast<llvm::ConstantInt>(little)) {
          return constant->isOne() ? raw : swap;
        }

        return this->lowering.builder.CreateSelect(little, raw, swap);
      };
      llvm::Value *result;
      if (!write) {
        llvm::LoadInst *raw =
            this->lowering.builder.CreateAlignedLoad(bits, address, llvm::Align(1));
        raw->setMetadata(llvm::LLVMContext::MD_tbaa,
                         this->lowering.bytesLowerer().bytesTbaa(false));
        llvm::Value *v = swapped(raw);
        switch (kind) {
          case Bytes::Float32:
            result = this->lowering.builder.CreateFPExt(
                this->lowering.builder.CreateBitCast(v, this->lowering.builder.getFloatTy()), f64);
            break;
          case Bytes::Float64:
            result = this->lowering.builder.CreateBitCast(v, f64);
            break;
          case Bytes::Float16:
            result = this->lowering.builder.CreateCall(this->fn("tz_f16_to_f64", f64, {i32}),
                                                       {this->lowering.builder.CreateZExt(v, i32)});
            break;
          default:
            result = BytesLowerer::isSignedKind(kind) ? this->lowering.builder.CreateSExt(v, i64)
                                                      : this->lowering.builder.CreateZExt(v, i64);
            break;
        }
      } else {
        llvm::Value *v;
        switch (kind) {
          case Bytes::Float32: {
            llvm::Value *top = llvm::ConstantFP::get(f64, kFloatTop);
            llvm::Value *within = this->lowering.builder.CreateBinaryIntrinsic(
                llvm::Intrinsic::minnum,
                this->lowering.builder.CreateBinaryIntrinsic(
                    llvm::Intrinsic::maxnum, value, llvm::ConstantFP::get(f64, -kFloatTop)),
                top);
            v = this->lowering.builder.CreateBitCast(
                this->lowering.builder.CreateFPTrunc(within, this->lowering.builder.getFloatTy()),
                bits);
            break;
          }
          case Bytes::Float64:
            v = this->lowering.builder.CreateBitCast(value, bits);
            break;
          case Bytes::Float16:
            v = this->lowering.builder.CreateTrunc(
                this->lowering.builder.CreateCall(this->fn("tz_f16_from_f64", i32, {f64}), {value}),
                bits);
            break;
          default:
            v = value->getType() == bits ? value : this->lowering.builder.CreateTrunc(value, bits);
            break;
        }
        llvm::StoreInst *store =
            this->lowering.builder.CreateAlignedStore(swapped(v), address, llvm::Align(1));
        store->setMetadata(llvm::LLVMContext::MD_tbaa,
                           this->lowering.bytesLowerer().bytesTbaa(false));
        result = dataView ? nullptr
                          : this->lowering.builder.CreateAdd(offset,
                                                             this->lowering.builder.getInt64(size));
      }
      this->lowering.refCounts().dropOperand(self);
      if (!write && call->type->isResult()) {
        llvm::Value *checked = this->lowering.builder.CreateCall(
            this->fn("tz_finite_r", f64, {f64, this->lowering.builder.getPtrTy()}),
            {result, this->lowering.resultLowerer().failureMaker(call->type->errType())});
        return this->lowering.resultLowerer().catchFailure(checked, call->type, call->loc());
      }
      return result;
    }
    case Builtin::BufferReadVar:
    case Builtin::BufferWriteVar: {
      bool write = call->builtin() == Builtin::BufferWriteVar;
      Lowering::Operand self = this->receiver(2);
      std::vector<llvm::Value *> values;
      for (size_t i = 2; i < call->args.size(); ++i) {
        values.push_back(this->lowering.exprLowerer().emitExpr(call->args[i]));
      }
      uint32_t flags = uint32_t(cast<BoolLitExpr>(call->args[0])->value) |
                       uint32_t(cast<BoolLitExpr>(call->args[1])->value) << 1;
      llvm::Value *result;
      if (write) {
        result = this->lowering.builder.CreateCall(
            this->fn("tz_buffer_write_var", i64, {ptr, i64, i64, i64, i32}),
            {self.value, values[0], values[1], values[2], this->lowering.builder.getInt32(flags)});
      } else {
        result = this->lowering.builder.CreateCall(
            this->fn("tz_buffer_read_var", i64, {ptr, i64, i64, i32}),
            {self.value, values[0], values[1], this->lowering.builder.getInt32(flags)});
      }
      this->lowering.refCounts().dropOperand(self);
      return result;
    }
    default:
      llvm_unreachable("not a call of NumberAccessCalls");
  }
}

}
