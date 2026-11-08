#include "compiler/lower/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/IR/MDBuilder.h"
#include <numeric>

using llvm::cast;

namespace tz {

namespace {

enum BytesField : unsigned { kRefCount, kLayout, kData, kLength, kOffset, kBuffer };

struct EncodingTable {
  const char *name;
  uint16_t high[128];
};

#define TZ_ENCODING_TABLE(name, ...) {name, {__VA_ARGS__}},

const EncodingTable kEncodingTables[] = {TZ_ENCODING_TABLES(TZ_ENCODING_TABLE)};

#undef TZ_ENCODING_TABLE

}

llvm::StructType *BytesLowerer::bytesStruct(llvm::LLVMContext &ctx) {
  llvm::Type *ptr = llvm::PointerType::getUnqual(ctx);
  llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);

  return llvm::StructType::get(ctx, {i64, ptr, ptr, i32, i32, ptr});
}

llvm::Type *BytesLowerer::storageType(llvm::IRBuilder<> &b, Bytes kind) {
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

bool BytesLowerer::isSignedKind(Bytes kind) {
  return kind == Bytes::Int8 || kind == Bytes::Int16 || kind == Bytes::Int32;
}

llvm::Value *BytesLowerer::toStorage(Lowering &, llvm::IRBuilder<> &b, Bytes kind,
                                     llvm::Value *value, llvm::FunctionCallee f16) {
  llvm::Type *storage = BytesLowerer::storageType(b, kind);
  switch (kind) {
  case Bytes::Uint8Clamped: {
    llvm::Value *zero = llvm::ConstantInt::get(value->getType(), 0);
    llvm::Value *max = llvm::ConstantInt::get(value->getType(), 255);
    llvm::Value *low = b.CreateSelect(b.CreateICmpSLT(value, zero), zero, value);
    return b.CreateTrunc(b.CreateSelect(b.CreateICmpSGT(low, max), max, low), storage);
  }
  case Bytes::Float64:
    return value;
  case Bytes::Float32: {
    llvm::Type *f64 = value->getType();
    llvm::Value *top = llvm::ConstantFP::get(f64, 3.4028234663852886e38);
    llvm::Value *within = b.CreateBinaryIntrinsic(
        llvm::Intrinsic::minnum,
        b.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, value,
                                llvm::ConstantFP::get(f64, -3.4028234663852886e38)),
        top);
    return b.CreateFPTrunc(within, storage);
  }
  case Bytes::Float16:
    return b.CreateTrunc(b.CreateCall(f16, {value}), storage);
  default:
    return value->getType() == storage ? value : b.CreateTrunc(value, storage);
  }
}

BytesLowerer::BytesLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::MDNode *BytesLowerer::bytesTbaa(bool header) {
  if (!this->lowering.tbaaHeader) {
    llvm::MDBuilder md(this->lowering.ctx);
    llvm::MDNode *root = md.createTBAARoot("tz bytes");
    llvm::MDNode *headerType = md.createTBAAScalarTypeNode("tz bytes header", root);
    llvm::MDNode *dataType = md.createTBAAScalarTypeNode("tz bytes data", root);
    this->lowering.tbaaHeader = md.createTBAAStructTagNode(headerType, headerType, 0);
    this->lowering.tbaaData = md.createTBAAStructTagNode(dataType, dataType, 0);
  }

  return header ? this->lowering.tbaaHeader : this->lowering.tbaaData;
}

llvm::Value *BytesLowerer::bytesData(llvm::Value *bytes) {
  if (!this->lowering.bytesTy) {
    this->lowering.bytesTy = BytesLowerer::bytesStruct(this->lowering.ctx);
  }
  llvm::LoadInst *data = this->lowering.builder.CreateLoad(
      this->lowering.builder.getPtrTy(),
      this->lowering.builder.CreateStructGEP(this->lowering.bytesTy, bytes, kData), "bytes.data");
  data->setMetadata(llvm::LLVMContext::MD_tbaa, this->bytesTbaa(true));

  return data;
}

llvm::Value *BytesLowerer::bytesLength(llvm::Value *bytes) {
  if (!this->lowering.bytesTy) {
    this->lowering.bytesTy = BytesLowerer::bytesStruct(this->lowering.ctx);
  }
  llvm::LoadInst *length = this->lowering.builder.CreateLoad(
      this->lowering.builder.getInt32Ty(),
      this->lowering.builder.CreateStructGEP(this->lowering.bytesTy, bytes, kLength),
      "bytes.length");
  length->setMetadata(llvm::LLVMContext::MD_tbaa, this->bytesTbaa(true));

  return this->lowering.builder.CreateZExt(length, this->lowering.builder.getInt64Ty());
}

llvm::Value *BytesLowerer::loadElement(llvm::Value *data, const Type *type, llvm::Value *index) {
  Bytes kind = type->bytes();
  llvm::Type *storage = BytesLowerer::storageType(this->lowering.builder, kind);
  llvm::LoadInst *raw = this->lowering.builder.CreateAlignedLoad(
      storage, this->lowering.builder.CreateInBoundsGEP(storage, data, index),
      llvm::Align(type->elementSize()), "element");
  raw->setMetadata(llvm::LLVMContext::MD_tbaa, this->bytesTbaa(false));
  switch (kind) {
  case Bytes::Float64:
    return raw;
  case Bytes::Float32:
    return this->lowering.builder.CreateFPExt(raw, this->lowering.builder.getDoubleTy());
  case Bytes::Float16:
    return this->lowering.builder.CreateCall(
        this->lowering.processLowerer().processFunction("tz_f16_to_f64",
                                                        this->lowering.builder.getDoubleTy(),
                                                        {this->lowering.builder.getInt32Ty()}),
        {this->lowering.builder.CreateZExt(raw, this->lowering.builder.getInt32Ty())});
  default:
    return raw;
  }
}

void BytesLowerer::storeElement(llvm::Value *data, const Type *type, llvm::Value *index,
                                llvm::Value *value) {
  Bytes kind = type->bytes();
  llvm::Type *storage = BytesLowerer::storageType(this->lowering.builder, kind);
  llvm::FunctionCallee f16 = kind == Bytes::Float16
                                 ? this->lowering.processLowerer().processFunction(
                                       "tz_f16_from_f64", this->lowering.builder.getInt32Ty(),
                                       {this->lowering.builder.getDoubleTy()})
                                 : llvm::FunctionCallee();
  llvm::StoreInst *store = this->lowering.builder.CreateAlignedStore(
      BytesLowerer::toStorage(this->lowering, this->lowering.builder, kind, value, f16),
      this->lowering.builder.CreateInBoundsGEP(storage, data, index),
      llvm::Align(type->elementSize()));
  store->setMetadata(llvm::LLVMContext::MD_tbaa, this->bytesTbaa(false));
}

llvm::Value *BytesLowerer::emitBytesIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  Lowering::Operand bytes = this->lowering.refCounts().emitOperand(
      index->object, AssignLowerer::cannotAssign(index->index));
  llvm::Value *position = this->lowering.exprLowerer().emitExpr(index->index);
  auto read = [&] { return this->loadElement(this->bytesData(bytes.value), type, position); };
  llvm::Value *result;
  if (!index->mayBeOutOfRange()) {
    result = read();
  } else {
    result = this->lowering.arrayLowerer().emitCheckedRead(
        this->lowering.builder.CreateICmpULT(position, this->bytesLength(bytes.value)), index->type,
        read);
  }
  this->lowering.refCounts().dropOperand(bytes);

  return result;
}

llvm::Value *BytesLowerer::emitBytesElementAssign(AssignExpr *assign, bool needResult) {
  auto *index = cast<IndexExpr>(assign->target);
  const Type *type = index->object->type;
  const Type *element = index->type;
  Lowering::Operand bytes = this->lowering.refCounts().emitOperand(
      index->object,
      AssignLowerer::cannotAssign(index->index) && AssignLowerer::cannotAssign(assign->value));
  llvm::Value *position = this->lowering.exprLowerer().emitExpr(index->index);
  llvm::Value *value;
  if (!assign->op) {
    value = this->lowering.exprLowerer().emitExpr(assign->value);
  } else {
    llvm::Value *old = this->loadElement(this->bytesData(bytes.value), type, position);
    value = this->lowering.binaryLowerer().emitArithmetic(
        *assign->op, old, this->lowering.exprLowerer().emitExpr(assign->value), element,
        assign->rangeProven);
  }
  this->storeElement(this->bytesData(bytes.value), type, position, value);
  this->lowering.refCounts().dropOperand(bytes);

  return needResult ? value : nullptr;
}

llvm::Value *BytesLowerer::emitBytesUpdate(UpdateExpr *update) {
  auto *index = cast<IndexExpr>(update->target);
  const Type *type = index->object->type;
  Lowering::Operand bytes = this->lowering.refCounts().emitOperand(
      index->object, AssignLowerer::cannotAssign(index->index));
  llvm::Value *position = this->lowering.exprLowerer().emitExpr(index->index);
  llvm::Value *data = this->bytesData(bytes.value);
  llvm::Value *old = this->loadElement(data, type, position);
  llvm::Value *updated;
  if (old->getType()->isDoubleTy()) {
    llvm::Value *one = llvm::ConstantFP::get(old->getType(), 1.0);
    updated = update->increment ? this->lowering.builder.CreateFAdd(old, one)
                                : this->lowering.builder.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(old->getType(), 1);
    updated = update->increment ? this->lowering.builder.CreateAdd(old, one)
                                : this->lowering.builder.CreateSub(old, one);
  }
  this->storeElement(data, type, position, updated);
  this->lowering.refCounts().dropOperand(bytes);

  return update->prefix ? updated : old;
}

llvm::Value *BytesLowerer::emitBytesMember(MemberExpr *member) {
  Lowering::Operand object = this->lowering.refCounts().emitOperand(member->object, true);
  llvm::Value *length = this->bytesLength(object.value);
  this->lowering.refCounts().dropOperand(object);

  return length;
}

Lowering::Producer BytesLowerer::bytesProducer(llvm::Value *slot, const Type *type) {
  Lowering::Producer producer;
  producer.run = [this, slot, type](const Lowering::Consumer &consumer) {
    llvm::Type *i64 = this->lowering.builder.getInt64Ty();
    llvm::Value *indexSlot = this->lowering.functionFrame().createStackSlot(i64, "index");
    this->lowering.builder.CreateStore(this->lowering.builder.getInt64(0), indexSlot);
    llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("iter.bytes");
    llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("iter.bytes.body");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("iter.bytes.end");
    this->lowering.functionFrame().emitBlock(headBlock);
    llvm::Value *bytes =
        this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(), slot, "bytes");
    llvm::Value *index = this->lowering.builder.CreateLoad(i64, indexSlot, "index");
    this->lowering.builder.CreateCondBr(
        this->lowering.builder.CreateICmpULT(index, this->bytesLength(bytes)), bodyBlock, endBlock);
    this->lowering.functionFrame().switchTo(bodyBlock);
    llvm::Value *value = this->loadElement(this->bytesData(bytes), type, index);
    this->lowering.builder.CreateStore(
        this->lowering.builder.CreateAdd(index, this->lowering.builder.getInt64(1)), indexSlot);
    if (llvm::Value *sent = consumer.each(value)) {
      this->lowering.refCounts().releaseValue(sent, consumer.sent);
    }
    this->lowering.functionFrame().branchTo(headBlock);
    this->lowering.functionFrame().switchTo(endBlock);
  };

  return producer;
}

llvm::Constant *BytesLowerer::encodingTable(llvm::StringRef name) {
  llvm::Constant *&table = this->encodingTables[name];
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
  auto *highTy = llvm::ArrayType::get(this->lowering.builder.getInt16Ty(), 128);
  std::vector<llvm::Constant *> high;
  for (uint16_t unit : found->high) {
    high.push_back(this->lowering.builder.getInt16(unit));
  }
  llvm::Constant *init =
      llvm::ConstantStruct::getAnon({this->lowering.stringLowerer().utf8Constant(found->name),
                                     llvm::ConstantArray::get(highTy, high)});
  auto *global =
      new llvm::GlobalVariable(*this->lowering.module_, init->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.encoding." + name);
  table = global;

  return table;
}

}
