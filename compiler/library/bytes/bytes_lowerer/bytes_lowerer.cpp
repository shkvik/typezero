#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
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

// TzBytes; an ArrayBuffer has data and length at the same places.
enum BytesField : unsigned { kRefCount, kLayout, kData, kLength, kOffset, kBuffer };

// The tables of the single-byte encodings, of which a program holds those
// it names (docs/bytes.md, section 5).
struct EncodingTable {
  const char *name;
  uint16_t high[128];
};

#define TZ_ENCODING_TABLE(name, ...) {name, {__VA_ARGS__}},

const EncodingTable kEncodingTables[] = {TZ_ENCODING_TABLES(TZ_ENCODING_TABLE)};

#undef TZ_ENCODING_TABLE

llvm::StructType *bytesStruct(llvm::LLVMContext &ctx) {
  llvm::Type *ptr = llvm::PointerType::getUnqual(ctx);
  llvm::Type *i32 = llvm::Type::getInt32Ty(ctx), *i64 = llvm::Type::getInt64Ty(ctx);

  return llvm::StructType::get(ctx, {i64, ptr, ptr, i32, i32, ptr});
}

// How an element lies in memory: an integer of its size (a Float16 as its
// bits), or a float.
llvm::Type *storageType(llvm::IRBuilder<> &b, Bytes kind) {
  switch (kind) {
  case Bytes::Int16:
  case Bytes::Uint16:
  case Bytes::Float16:
    return b.getInt16Ty();
  case Bytes::Int32:
  case Bytes::Uint32:
    return b.getInt32Ty();
  case Bytes::Float32:
    return b.getFloatTy();
  case Bytes::Float64:
    return b.getDoubleTy();
  default:
    return b.getInt8Ty();
  }
}

bool isSignedKind(Bytes kind) {
  return kind == Bytes::Int8 || kind == Bytes::Int16 || kind == Bytes::Int32;
}

// A value of the element type of `kind` as the bits it is stored as: cut to
// the size (Checker proved it fits), clamped, or rounded to the float.
llvm::Value *toStorage(Lowering &, llvm::IRBuilder<> &b, Bytes kind, llvm::Value *value,
                       llvm::FunctionCallee f16) {
  llvm::Type *storage = storageType(b, kind);
  switch (kind) {
  case Bytes::Uint8Clamped: {
    llvm::Value *zero = b.getInt32(0), *max = b.getInt32(255);
    llvm::Value *low = b.CreateSelect(b.CreateICmpSLT(value, zero), zero, value);
    return b.CreateTrunc(b.CreateSelect(b.CreateICmpSGT(low, max), max, low), storage);
  }
  case Bytes::Float64:
    return value;
  case Bytes::Float32:
    return b.CreateFPTrunc(value, storage);
  case Bytes::Float16:
    return b.CreateTrunc(b.CreateCall(f16, {value}), storage);
  default:
    return value->getType() == storage ? value : b.CreateTrunc(value, storage);
  }
}

} // namespace

BytesLowerer::BytesLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::MDNode *BytesLowerer::bytesTbaa(bool header) {
  if (!lowering_.tbaaHeader_) {
    llvm::MDBuilder md(lowering_.ctx_);
    llvm::MDNode *root = md.createTBAARoot("tz bytes");
    llvm::MDNode *headerType = md.createTBAAScalarTypeNode("tz bytes header", root);
    llvm::MDNode *dataType = md.createTBAAScalarTypeNode("tz bytes data", root);
    lowering_.tbaaHeader_ = md.createTBAAStructTagNode(headerType, headerType, 0);
    lowering_.tbaaData_ = md.createTBAAStructTagNode(dataType, dataType, 0);
  }

  return header ? lowering_.tbaaHeader_ : lowering_.tbaaData_;
}

llvm::Value *BytesLowerer::bytesData(llvm::Value *bytes) {
  if (!lowering_.bytesTy_) {
    lowering_.bytesTy_ = bytesStruct(lowering_.ctx_);
  }
  llvm::LoadInst *data = lowering_.builder_.CreateLoad(
      lowering_.builder_.getPtrTy(),
      lowering_.builder_.CreateStructGEP(lowering_.bytesTy_, bytes, kData), "bytes.data");
  data->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/true));

  return data;
}

llvm::Value *BytesLowerer::bytesLength(llvm::Value *bytes) {
  if (!lowering_.bytesTy_) {
    lowering_.bytesTy_ = bytesStruct(lowering_.ctx_);
  }
  llvm::LoadInst *length = lowering_.builder_.CreateLoad(
      lowering_.builder_.getInt32Ty(),
      lowering_.builder_.CreateStructGEP(lowering_.bytesTy_, bytes, kLength), "bytes.length");
  length->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/true));

  return lowering_.builder_.CreateZExt(length, lowering_.builder_.getInt64Ty());
}

llvm::Value *BytesLowerer::loadElement(llvm::Value *data, const Type *type, llvm::Value *index) {
  Bytes kind = type->bytes();
  llvm::Type *storage = storageType(lowering_.builder_, kind);
  llvm::LoadInst *raw = lowering_.builder_.CreateAlignedLoad(
      storage, lowering_.builder_.CreateInBoundsGEP(storage, data, index),
      llvm::Align(type->elementSize()), "element");
  raw->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/false));
  switch (kind) {
  case Bytes::Uint32:
    return lowering_.builder_.CreateZExt(raw, lowering_.builder_.getInt64Ty());
  case Bytes::Int32:
  case Bytes::Float64:
    return raw;
  case Bytes::Float32:
    return lowering_.builder_.CreateFPExt(raw, lowering_.builder_.getDoubleTy());
  case Bytes::Float16:
    return lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction(
            "tz_f16_to_f64", lowering_.builder_.getDoubleTy(), {lowering_.builder_.getInt32Ty()}),
        {lowering_.builder_.CreateZExt(raw, lowering_.builder_.getInt32Ty())});
  default:
    return isSignedKind(kind) ? lowering_.builder_.CreateSExt(raw, lowering_.builder_.getInt32Ty())
                              : lowering_.builder_.CreateZExt(raw, lowering_.builder_.getInt32Ty());
  }
}

void BytesLowerer::storeElement(llvm::Value *data, const Type *type, llvm::Value *index,
                                llvm::Value *value) {
  Bytes kind = type->bytes();
  llvm::Type *storage = storageType(lowering_.builder_, kind);
  llvm::FunctionCallee f16 = kind == Bytes::Float16
                                 ? lowering_.processLowerer().processFunction(
                                       "tz_f16_from_f64", lowering_.builder_.getInt32Ty(),
                                       {lowering_.builder_.getDoubleTy()})
                                 : llvm::FunctionCallee();
  llvm::StoreInst *store = lowering_.builder_.CreateAlignedStore(
      toStorage(lowering_, lowering_.builder_, kind, value, f16),
      lowering_.builder_.CreateInBoundsGEP(storage, data, index), llvm::Align(type->elementSize()));
  store->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/false));
}

llvm::Value *BytesLowerer::emitBytesIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  Lowering::Operand bytes =
      lowering_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
  lowering_.throwLowerer().holdOperand(bytes, lowering_.mayThrow().exprThrows(index->index));
  llvm::Value *position = lowering_.exprLowerer().emitExpr(index->index);
  lowering_.throwLowerer().takeOperand(bytes);
  auto read = [&] { return loadElement(bytesData(bytes.value), type, position); };
  llvm::Value *result;
  if (!index->mayBeOutOfRange) {
    result = read();
  } else {
    result = lowering_.arrayLowerer().emitCheckedRead(
        lowering_.builder_.CreateICmpULT(position, bytesLength(bytes.value)), index->type, read);
  }
  lowering_.refCounts().dropOperand(bytes);

  return result;
}

// `u[i] = v` and `u[i] op= v`: Checker proved the index in range and the value
// to fit.
llvm::Value *BytesLowerer::emitBytesElementAssign(AssignExpr *assign, bool needResult) {
  auto *index = cast<IndexExpr>(assign->target);
  const Type *type = index->object->type;
  const Type *element = index->type;
  Lowering::Operand bytes = lowering_.refCounts().emitOperand(
      index->object,
      AssignLowerer::cannotAssign(index->index) && AssignLowerer::cannotAssign(assign->value));
  lowering_.throwLowerer().holdOperand(bytes, lowering_.mayThrow().exprThrows(index->index) ||
                                                  lowering_.mayThrow().exprThrows(assign->value));
  llvm::Value *position = lowering_.exprLowerer().emitExpr(index->index);
  llvm::Value *value;
  if (!assign->op) {
    value = lowering_.exprLowerer().emitExpr(assign->value);
  } else {
    // As in JavaScript, the old value is read before the right side runs.
    llvm::Value *old = loadElement(bytesData(bytes.value), type, position);
    value = lowering_.binaryLowerer().emitArithmetic(
        *assign->op, old, lowering_.exprLowerer().emitExpr(assign->value), element);
  }
  lowering_.throwLowerer().takeOperand(bytes);
  storeElement(bytesData(bytes.value), type, position, value);
  lowering_.refCounts().dropOperand(bytes);

  return needResult ? value : nullptr;
}

llvm::Value *BytesLowerer::emitBytesUpdate(UpdateExpr *update) {
  auto *index = cast<IndexExpr>(update->target);
  const Type *type = index->object->type;
  Lowering::Operand bytes =
      lowering_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
  lowering_.throwLowerer().holdOperand(bytes, lowering_.mayThrow().exprThrows(index->index));
  llvm::Value *position = lowering_.exprLowerer().emitExpr(index->index);
  lowering_.throwLowerer().takeOperand(bytes);
  llvm::Value *data = bytesData(bytes.value);
  llvm::Value *old = loadElement(data, type, position);
  llvm::Value *updated;
  if (old->getType()->isDoubleTy()) {
    llvm::Value *one = llvm::ConstantFP::get(old->getType(), 1.0);
    updated = update->increment ? lowering_.builder_.CreateFAdd(old, one)
                                : lowering_.builder_.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(old->getType(), 1);
    updated = update->increment ? lowering_.builder_.CreateAdd(old, one)
                                : lowering_.builder_.CreateSub(old, one);
  }
  storeElement(data, type, position, updated);
  lowering_.refCounts().dropOperand(bytes);

  return update->prefix ? updated : old;
}

llvm::Value *BytesLowerer::emitBytesMember(MemberExpr *member) {
  // `.length` of a typed array, `.byteLength` of a DataView or an
  // ArrayBuffer: the length, where both keep it.
  Lowering::Operand object = lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *length = bytesLength(object.value);
  lowering_.refCounts().dropOperand(object);

  return lowering_.builder_.CreateTrunc(length, lowering_.builder_.getInt32Ty(), "length");
}

Lowering::Producer BytesLowerer::bytesProducer(llvm::Value *slot, const Type *type) {
  Lowering::Producer producer;
  producer.run = [this, slot, type](const Lowering::Consumer &consumer) {
    llvm::Type *i64 = lowering_.builder_.getInt64Ty();
    llvm::Value *indexSlot = lowering_.functionFrame().createStackSlot(i64, "index");
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(0), indexSlot);
    llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("iter.bytes");
    llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("iter.bytes.body");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("iter.bytes.end");
    lowering_.functionFrame().emitBlock(headBlock);
    llvm::Value *bytes =
        lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot, "bytes");
    llvm::Value *index = lowering_.builder_.CreateLoad(i64, indexSlot, "index");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(index, bytesLength(bytes)),
                                    bodyBlock, endBlock);
    lowering_.functionFrame().switchTo(bodyBlock);
    llvm::Value *value = loadElement(bytesData(bytes), type, index);
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateAdd(index, lowering_.builder_.getInt64(1)), indexSlot);
    if (llvm::Value *sent = consumer.each(value)) {
      lowering_.refCounts().releaseValue(sent, consumer.sent);
    }
    lowering_.functionFrame().branchTo(headBlock);
    lowering_.functionFrame().switchTo(endBlock);
  };

  return producer;
}

llvm::Constant *BytesLowerer::encodingTable(llvm::StringRef name) {
  llvm::Constant *&table = encodingTables_[name];
  if (table) {
    return table;
  }
  const EncodingTable *found = nullptr;
  for (const EncodingTable &t : kEncodingTables) {
    if (name == t.name) {
      found = &t;
    }
  }
  assert(found && "Checker checked the encoding");
  auto *highTy = llvm::ArrayType::get(lowering_.builder_.getInt16Ty(), 128);
  std::vector<llvm::Constant *> high;
  for (uint16_t unit : found->high) {
    high.push_back(lowering_.builder_.getInt16(unit));
  }
  llvm::Constant *init =
      llvm::ConstantStruct::getAnon({lowering_.stringLowerer().utf8Constant(found->name),
                                     llvm::ConstantArray::get(highTy, high)});
  auto *global =
      new llvm::GlobalVariable(*lowering_.module_, init->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.encoding." + name);
  table = global;

  return table;
}

} // namespace tz
