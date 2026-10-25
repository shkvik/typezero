// Binary data (docs/bytes.md). A typed array, a Buffer or a DataView is a
// pointer to a TzBytes of the runtime (runtime/core/tzrt/tzrt.h):
//   { i64 rc, ptr layout, ptr data, i32 length, i32 offset, ptr buffer }
// and an ArrayBuffer a pointer to { i64 rc, ptr layout, ptr data, i32 length,
// i32 flags }. The runtime makes them (runtime/library/bytes/bytes/bytes.c,
// buffer.c); the code reads and writes the elements itself, at `data`.
//
// `data` and `length` never change, but a store of an element could, as far
// as LLVM knows, write over them, and then a loop that writes elements would
// load them again on each step and could not be vectorized. So their loads
// carry TBAA of their own (bytesTbaa), apart from the accesses of elements:
// LLVM keeps them in registers, and the loops over bytes become vector code.

#include "compiler/lower/codegen/codegen.h"

#include "runtime/library/bytes/encodings/encodings.h"

#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

using Bytes = Type::Bytes;

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

// The encodings of TextDecoder, numbered as in
// runtime/library/bytes/buffer/buffer.c.
enum { kTextUtf8, kTextUtf16le, kTextUtf16be, kTextSingleByte, kTextCjk };

// The multi-byte encodings (runtime/library/bytes/cjk/cjk.c), each a
// TzCjkEncoding of the runtime that names its indexes: a program that refers to
// one gets those from the linker, and no others.
const char *const kCjkEncodings[] = {"gbk",         "gb18030",   "big5",  "euc-jp",
                                     "iso-2022-jp", "shift_jis", "euc-kr"};

llvm::Constant *cjkEncodingOf(llvm::Module &module, llvm::StringRef name) {
  std::string symbol = "tz_cjk_encoding_" + name.str();
  for (char &c : symbol)
    if (c == '-')
      c = '_';
  return module.getOrInsertGlobal(symbol, llvm::Type::getInt8Ty(module.getContext()));
}

int64_t constantOf(const Expr *e) { return int64_t(cast<IntLitExpr>(e)->value); }

bool isConstant(const Expr *e) { return isa<IntLitExpr>(e) || isa<BoolLitExpr>(e); }

} // namespace

bool bytesMayThrow(const CallExpr *call) {
  switch (call->builtin) {
  case Builtin::BytesNew:
  case Builtin::ArrayBufferNew: {
    // A length that is a constant and not negative cannot fail.
    const Expr *length = call->args[0];
    if (auto *conv = dyn_cast<CastExpr>(length); conv && conv->isImplicit())
      length = conv->operand;
    return !isa<IntLitExpr>(length);
  }
  case Builtin::BytesView:
  case Builtin::BytesSet:
  case Builtin::BytesWith:
  case Builtin::DataViewNew:
  case Builtin::BufferAllocFill:
  case Builtin::BufferConcat:
  case Builtin::BufferCompareTo:
  case Builtin::BufferWrite:
  case Builtin::BufferCopy:
  case Builtin::BufferFill:
  case Builtin::BufferReadVar:
  case Builtin::BufferWriteVar:
  case Builtin::BufferSwap:
  case Builtin::TextDecode:
    return true;
  case Builtin::DataViewGet:
  case Builtin::DataViewSet:
  case Builtin::BufferRead:
  case Builtin::BufferWriteNumber:
    return call->mayBeOutOfRange;
  case Builtin::TextDecoderNew:
    return constantOf(call->args[0]) == 0; // a label known only when it runs
  // An encoding known only when the program runs.
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

llvm::MDNode *CodeGen::bytesTbaa(bool header) {
  if (!tbaaHeader_) {
    llvm::MDBuilder md(ctx_);
    llvm::MDNode *root = md.createTBAARoot("tz bytes");
    llvm::MDNode *headerType = md.createTBAAScalarTypeNode("tz bytes header", root);
    llvm::MDNode *dataType = md.createTBAAScalarTypeNode("tz bytes data", root);
    tbaaHeader_ = md.createTBAAStructTagNode(headerType, headerType, 0);
    tbaaData_ = md.createTBAAStructTagNode(dataType, dataType, 0);
  }
  return header ? tbaaHeader_ : tbaaData_;
}

namespace {

llvm::StructType *bytesStruct(llvm::LLVMContext &ctx) {
  llvm::Type *ptr = llvm::PointerType::getUnqual(ctx);
  llvm::Type *i32 = llvm::Type::getInt32Ty(ctx), *i64 = llvm::Type::getInt64Ty(ctx);
  return llvm::StructType::get(ctx, {i64, ptr, ptr, i32, i32, ptr});
}

} // namespace

llvm::Value *CodeGen::bytesData(llvm::Value *bytes) {
  if (!bytesTy_)
    bytesTy_ = bytesStruct(ctx_);
  llvm::LoadInst *data = builder_.CreateLoad(
      builder_.getPtrTy(), builder_.CreateStructGEP(bytesTy_, bytes, kData), "bytes.data");
  data->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/true));
  return data;
}

llvm::Value *CodeGen::bytesLength(llvm::Value *bytes) {
  if (!bytesTy_)
    bytesTy_ = bytesStruct(ctx_);
  llvm::LoadInst *length = builder_.CreateLoad(
      builder_.getInt32Ty(), builder_.CreateStructGEP(bytesTy_, bytes, kLength), "bytes.length");
  length->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/true));
  return builder_.CreateZExt(length, builder_.getInt64Ty());
}

namespace {

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

} // namespace

llvm::Value *CodeGen::loadElement(llvm::Value *data, const Type *type, llvm::Value *index) {
  Bytes kind = type->bytes();
  llvm::Type *storage = storageType(builder_, kind);
  llvm::LoadInst *raw = builder_.CreateAlignedLoad(
      storage, builder_.CreateInBoundsGEP(storage, data, index),
      llvm::Align(type->elementSize()), "element");
  raw->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/false));
  switch (kind) {
  case Bytes::Uint32:
    return builder_.CreateZExt(raw, builder_.getInt64Ty());
  case Bytes::Int32:
  case Bytes::Float64:
    return raw;
  case Bytes::Float32:
    return builder_.CreateFPExt(raw, builder_.getDoubleTy());
  case Bytes::Float16:
    return builder_.CreateCall(
        processFunction("tz_f16_to_f64", builder_.getDoubleTy(), {builder_.getInt32Ty()}),
        {builder_.CreateZExt(raw, builder_.getInt32Ty())});
  default:
    return isSignedKind(kind) ? builder_.CreateSExt(raw, builder_.getInt32Ty())
                              : builder_.CreateZExt(raw, builder_.getInt32Ty());
  }
}

namespace {

// A value of the element type of `kind` as the bits it is stored as: cut to
// the size (Sema proved it fits), clamped, or rounded to the float.
llvm::Value *toStorage(CodeGen &, llvm::IRBuilder<> &b, Bytes kind, llvm::Value *value,
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

void CodeGen::storeElement(llvm::Value *data, const Type *type, llvm::Value *index,
                           llvm::Value *value) {
  Bytes kind = type->bytes();
  llvm::Type *storage = storageType(builder_, kind);
  llvm::FunctionCallee f16 =
      kind == Bytes::Float16
          ? processFunction("tz_f16_from_f64", builder_.getInt32Ty(), {builder_.getDoubleTy()})
          : llvm::FunctionCallee();
  llvm::StoreInst *store = builder_.CreateAlignedStore(
      toStorage(*this, builder_, kind, value, f16),
      builder_.CreateInBoundsGEP(storage, data, index), llvm::Align(type->elementSize()));
  store->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/false));
}

//===----------------------------------------------------------------------===//
// Elements
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitBytesIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  Operand bytes = emitOperand(index->object, cannotAssign(index->index));
  holdOperand(bytes, exprThrows(index->index));
  llvm::Value *position = emitExpr(index->index);
  takeOperand(bytes);
  auto read = [&] { return loadElement(bytesData(bytes.value), type, position); };
  llvm::Value *result;
  if (!index->mayBeOutOfRange)
    result = read();
  else
    result = emitCheckedRead(builder_.CreateICmpULT(position, bytesLength(bytes.value)),
                             index->type, read);
  dropOperand(bytes);
  return result;
}

// `u[i] = v` and `u[i] op= v`: Sema proved the index in range and the value
// to fit.
llvm::Value *CodeGen::emitBytesElementAssign(AssignExpr *assign, bool needResult) {
  auto *index = cast<IndexExpr>(assign->target);
  const Type *type = index->object->type;
  const Type *element = index->type;
  Operand bytes =
      emitOperand(index->object, cannotAssign(index->index) && cannotAssign(assign->value));
  holdOperand(bytes, exprThrows(index->index) || exprThrows(assign->value));
  llvm::Value *position = emitExpr(index->index);
  llvm::Value *value;
  if (!assign->op) {
    value = emitExpr(assign->value);
  } else {
    // As in JavaScript, the old value is read before the right side runs.
    llvm::Value *old = loadElement(bytesData(bytes.value), type, position);
    value = emitArithmetic(*assign->op, old, emitExpr(assign->value), element);
  }
  takeOperand(bytes);
  storeElement(bytesData(bytes.value), type, position, value);
  dropOperand(bytes);
  return needResult ? value : nullptr;
}

llvm::Value *CodeGen::emitBytesUpdate(UpdateExpr *update) {
  auto *index = cast<IndexExpr>(update->target);
  const Type *type = index->object->type;
  Operand bytes = emitOperand(index->object, cannotAssign(index->index));
  holdOperand(bytes, exprThrows(index->index));
  llvm::Value *position = emitExpr(index->index);
  takeOperand(bytes);
  llvm::Value *data = bytesData(bytes.value);
  llvm::Value *old = loadElement(data, type, position);
  llvm::Value *updated;
  if (old->getType()->isDoubleTy()) {
    llvm::Value *one = llvm::ConstantFP::get(old->getType(), 1.0);
    updated = update->increment ? builder_.CreateFAdd(old, one) : builder_.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(old->getType(), 1);
    updated = update->increment ? builder_.CreateAdd(old, one) : builder_.CreateSub(old, one);
  }
  storeElement(data, type, position, updated);
  dropOperand(bytes);
  return update->prefix ? updated : old;
}

llvm::Value *CodeGen::emitBytesMember(MemberExpr *member) {
  // `.length` of a typed array, `.byteLength` of a DataView or an
  // ArrayBuffer: the length, where both keep it.
  Operand object = emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *length = bytesLength(object.value);
  dropOperand(object);
  return builder_.CreateTrunc(length, builder_.getInt32Ty(), "length");
}

CodeGen::Producer CodeGen::bytesProducer(llvm::Value *slot, const Type *type) {
  Producer producer;
  producer.run = [this, slot, type](const Consumer &consumer) {
    llvm::Type *i64 = builder_.getInt64Ty();
    llvm::Value *indexSlot = createStackSlot(i64, "index");
    builder_.CreateStore(builder_.getInt64(0), indexSlot);
    llvm::BasicBlock *headBlock = newBlock("iter.bytes");
    llvm::BasicBlock *bodyBlock = newBlock("iter.bytes.body");
    llvm::BasicBlock *endBlock = newBlock("iter.bytes.end");
    emitBlock(headBlock);
    llvm::Value *bytes = builder_.CreateLoad(builder_.getPtrTy(), slot, "bytes");
    llvm::Value *index = builder_.CreateLoad(i64, indexSlot, "index");
    builder_.CreateCondBr(builder_.CreateICmpULT(index, bytesLength(bytes)), bodyBlock, endBlock);
    switchTo(bodyBlock);
    llvm::Value *value = loadElement(bytesData(bytes), type, index);
    builder_.CreateStore(builder_.CreateAdd(index, builder_.getInt64(1)), indexSlot);
    if (llvm::Value *sent = consumer.each(value))
      releaseValue(sent, consumer.sent);
    branchTo(headBlock);
    switchTo(endBlock);
  };
  return producer;
}

//===----------------------------------------------------------------------===//
// Callbacks
//===----------------------------------------------------------------------===//

// forEach, map, filter, reduce, reduceRight, find, findLast, findIndex,
// findLastIndex, some, every and sort(compare) of a typed array. Its length
// never changes, and its elements are numbers: nothing to count.
llvm::Value *CodeGen::emitBytesCallbackMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->elementType();
  const Type *callbackType = call->args[0]->type;
  llvm::Type *i64 = builder_.getInt64Ty(), *ptr = builder_.getPtrTy();
  Builtin method = call->builtin;
  bool throws = typeThrows(callbackType);
  bool reduce = method == Builtin::ArrReduce || method == Builtin::ArrReduceRight;
  bool emptyThrows = reduce && call->args.size() < 2;
  llvm::Value *array = emitExpr(member->object);
  Held heldArray = hold(array, arrayType,
                        throws || emptyThrows ||
                            llvm::any_of(call->args, [&](Expr *arg) { return exprThrows(arg); }));
  llvm::Value *callback = emitExpr(call->args[0]);
  Held heldCallback = hold(callback, callbackType,
                           throws || emptyThrows ||
                               (call->args.size() > 1 && exprThrows(call->args[1])));
  llvm::Value *init = call->args.size() > 1 ? emitExpr(call->args[1]) : nullptr;

  if (method == Builtin::ArrSortWith) {
    // toSorted(compare) sorts a copy.
    bool copy = member->name == "toSorted";
    llvm::Value *target = array;
    Held heldCopy{nullptr, nullptr, nullptr};
    if (copy) {
      target = builder_.CreateCall(
          processFunction("tz_bytes_slice", ptr, {ptr, i64, i64, builder_.getInt32Ty()}),
          {array, builder_.getInt64(0), builder_.getInt64(0), builder_.getInt32(0)}, "sorted");
      heldCopy = hold(target, call->type, throws);
    }
    llvm::Value *context = spill(callback, "compare");
    builder_.CreateCall(processFunction("tz_bytes_sort_with", builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {target, sortComparator(call), context});
    releaseValue(take(heldCallback), callbackType);
    emitThrowCheckIf(throws);
    if (!copy)
      return take(heldArray); // the same array, with this reference
    releaseValue(take(heldArray), arrayType);
    return take(heldCopy);
  }

  bool backward = method == Builtin::ArrReduceRight || method == Builtin::ArrFindLast ||
                  method == Builtin::ArrFindLastIndex;
  size_t passes = std::min(callbackType->params().size(), size_t(reduce ? 4 : 3));
  size_t valueArgs = reduce ? 2 : 1;
  llvm::Value *length = bytesLength(array);
  llvm::Value *start = backward ? builder_.CreateSub(length, builder_.getInt64(1)) : builder_.getInt64(0);
  llvm::Value *step = builder_.getInt64(backward ? -1 : 1);

  llvm::Value *accSlot = nullptr;
  if (reduce) {
    accSlot = createStackSlot(lower(call->type), "acc");
    if (init) {
      builder_.CreateStore(init, accSlot);
    } else {
      llvm::BasicBlock *emptyBlock = newBlock("reduce.empty");
      llvm::BasicBlock *firstBlock = newBlock("reduce.first");
      builder_.CreateCondBr(builder_.CreateICmpEQ(length, builder_.getInt64(0)), emptyBlock,
                            firstBlock, llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
      switchTo(emptyBlock);
      emitThrowNew(1, "Reduce of empty array with no initial value");
      switchTo(firstBlock);
      builder_.CreateStore(loadElement(bytesData(array), arrayType, start), accSlot);
      start = builder_.CreateAdd(start, step);
    }
  }
  // map: a new typed array of the same kind, of the same length; filter: an
  // array of the elements kept, which becomes one at the end.
  llvm::Value *out = nullptr;
  int32_t kind = int32_t(arrayType->bytes());
  if (method == Builtin::ArrMap)
    out = builder_.CreateCall(
        processFunction("tz_bytes_new_unset", ptr, {builder_.getInt32Ty(), i64}),
        {builder_.getInt32(uint32_t(kind)), length}, "mapped");
  else if (method == Builtin::ArrFilter)
    out = builder_.CreateCall(rt_.arrNew,
                              {typeDescriptor(element), builder_.getInt64(0), builder_.getInt64(0)},
                              "kept");
  Held heldOut = hold(out, method == Builtin::ArrMap ? call->type : Type::array(element), throws);
  llvm::Value *resultSlot = nullptr;
  if (!reduce && !out && method != Builtin::ArrForEach) {
    resultSlot = createStackSlot(lower(call->type), "result");
    llvm::Value *none = method == Builtin::ArrSome    ? builder_.getFalse()
                        : method == Builtin::ArrEvery ? builder_.getTrue()
                        : call->type->isInt()         ? builder_.getInt32(~0u)
                                                      : undefinedValue(call->type);
    builder_.CreateStore(none, resultSlot);
  }

  llvm::Value *indexSlot = createStackSlot(i64, "index");
  builder_.CreateStore(start, indexSlot);
  llvm::BasicBlock *headBlock = newBlock("each");
  llvm::BasicBlock *bodyBlock = newBlock("each.body");
  llvm::BasicBlock *nextBlock = newBlock("each.next");
  llvm::BasicBlock *endBlock = newBlock("each.end");
  emitBlock(headBlock);
  llvm::Value *i = builder_.CreateLoad(i64, indexSlot, "i");
  // Going back, the unsigned comparison stops below 0 too.
  builder_.CreateCondBr(builder_.CreateICmpULT(i, length), bodyBlock, endBlock);

  switchTo(bodyBlock);
  llvm::Value *value = loadElement(bytesData(array), arrayType, i);
  std::vector<llvm::Value *> args;
  if (reduce)
    args.push_back(builder_.CreateLoad(lower(call->type), accSlot));
  args.push_back(value);
  args.push_back(builder_.CreateTrunc(i, builder_.getInt32Ty()));
  args.push_back(array);
  if (reduce && passes == 0)
    releaseValue(args[0], call->type);
  args.resize(passes);
  if (passes == valueArgs + 2)
    retainValue(array, arrayType); // the callback owns its arguments
  llvm::Value *result = callFunctionValue(callback, callbackType, args);
  emitThrowCheckIf(throws);
  const Type *resultType = callbackType->ret();

  llvm::BasicBlock *foundBlock = nullptr;
  auto test = [&](llvm::Value *decides) {
    foundBlock = newBlock("each.found");
    llvm::BasicBlock *otherBlock = newBlock("each.other");
    builder_.CreateCondBr(decides, foundBlock, otherBlock);
    switchTo(otherBlock);
  };
  switch (method) {
  case Builtin::ArrForEach:
    if (result)
      releaseValue(result, resultType);
    break;
  case Builtin::ArrMap:
    storeElement(bytesData(out), arrayType, i, result);
    break;
  case Builtin::ArrFilter: {
    llvm::BasicBlock *keepBlock = newBlock("filter.keep");
    builder_.CreateCondBr(result, keepBlock, nextBlock);
    switchTo(keepBlock);
    emitPush(out, {value}, element);
    break;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
    builder_.CreateStore(result, accSlot);
    break;
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    test(result);
    builder_.CreateBr(nextBlock);
    switchTo(foundBlock);
    builder_.CreateStore(makeOptional(value, builder_.getTrue(), call->type), resultSlot);
    builder_.CreateBr(endBlock);
    switchTo(newBlock("dead"));
    break;
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    test(result);
    builder_.CreateBr(nextBlock);
    switchTo(foundBlock);
    builder_.CreateStore(builder_.CreateTrunc(i, builder_.getInt32Ty()), resultSlot);
    builder_.CreateBr(endBlock);
    switchTo(newBlock("dead"));
    break;
  case Builtin::ArrSome:
  case Builtin::ArrEvery: {
    bool some = method == Builtin::ArrSome;
    test(some ? result : builder_.CreateNot(result));
    builder_.CreateBr(nextBlock);
    switchTo(foundBlock);
    builder_.CreateStore(builder_.getInt1(some), resultSlot);
    builder_.CreateBr(endBlock);
    switchTo(newBlock("dead"));
    break;
  }
  default:
    llvm_unreachable("not a method with a callback");
  }
  branchTo(nextBlock);

  switchTo(nextBlock);
  builder_.CreateStore(builder_.CreateAdd(i, step), indexSlot);
  builder_.CreateBr(headBlock);

  switchTo(endBlock);
  releaseValue(take(heldCallback), callbackType);
  releaseValue(take(heldArray), arrayType);
  if (method == Builtin::ArrMap)
    return take(heldOut);
  if (method == Builtin::ArrFilter) {
    llvm::Value *kept = take(heldOut);
    llvm::Value *made = builder_.CreateCall(
        processFunction("tz_bytes_from_array", ptr, {builder_.getInt32Ty(), ptr}),
        {builder_.getInt32(uint32_t(kind)), kept}, "filtered");
    releaseValue(kept, Type::array(element));
    return made;
  }
  if (reduce)
    return builder_.CreateLoad(lower(call->type), accSlot);
  if (resultSlot)
    return builder_.CreateLoad(lower(call->type), resultSlot);
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Built-ins
//===----------------------------------------------------------------------===//

llvm::Constant *CodeGen::encodingTable(llvm::StringRef name) {
  llvm::Constant *&table = encodingTables_[name];
  if (table)
    return table;
  const EncodingTable *found = nullptr;
  for (const EncodingTable &t : kEncodingTables)
    if (name == t.name)
      found = &t;
  assert(found && "Sema checked the encoding");
  auto *highTy = llvm::ArrayType::get(builder_.getInt16Ty(), 128);
  std::vector<llvm::Constant *> high;
  for (uint16_t unit : found->high)
    high.push_back(builder_.getInt16(unit));
  llvm::Constant *init = llvm::ConstantStruct::getAnon(
      {utf8Constant(found->name), llvm::ConstantArray::get(highTy, high)});
  auto *global = new llvm::GlobalVariable(*module_, init->getType(), /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init,
                                          "tz.encoding." + name);
  table = global;
  return table;
}

llvm::Value *CodeGen::emitBytesCall(CallExpr *call) {
  llvm::Type *voidTy = builder_.getVoidTy(), *i32 = builder_.getInt32Ty(),
             *i64 = builder_.getInt64Ty(), *f64 = builder_.getDoubleTy(), *ptr = builder_.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Type *type = call->type;
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return processFunction(name, ret, params);
  };
  auto kindOf = [&](const Type *t) { return builder_.getInt32(uint32_t(t->bytes())); };
  bool throws = bytesMayThrow(call);
  // The receiver of a method, held while the arguments from `from` on are
  // evaluated if they (or an encoding among them) may throw; takeOperand
  // once they are.
  auto receiver = [&](size_t from) {
    auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
    Operand self = emitOperand(member->object, llvm::all_of(rest, cannotAssign));
    holdOperand(self, throws || llvm::any_of(rest, [&](Expr *arg) { return exprThrows(arg); }));
    return self;
  };
  // A method that gives the receiver back hands over its reference.
  auto returnSelf = [&](Operand &self) {
    if (!self.owned)
      retainValue(self.value, self.type);
    self.owned = false;
    return self.value;
  };
  // Optional positions from args[from] on: their values (0 when left out)
  // and a mask of those given, from bit 0 on.
  auto positions = [&](size_t from, size_t count, std::vector<llvm::Value *> &values) {
    uint32_t has = 0;
    for (size_t k = 0; k < count; ++k) {
      if (from + k < call->args.size()) {
        values.push_back(emitExpr(call->args[from + k]));
        has |= 1u << k;
      } else {
        values.push_back(builder_.getInt64(0));
      }
    }
    return builder_.getInt32(has);
  };
  // An encoding: a constant Sema found, or a string checked now.
  auto encoding = [&](Expr *arg) -> llvm::Value * {
    if (auto *lit = dyn_cast<IntLitExpr>(arg))
      return builder_.getInt32(uint32_t(lit->value));
    Operand name = emitOperand(arg, /*canBorrow=*/true);
    llvm::Value *value = builder_.CreateCall(fn("tz_buffer_encoding_checked", i32, {ptr, i64}),
                                             {stringObj(name.value), stringMeta(name.value)});
    dropOperand(name);
    emitThrowCheck(); // an unknown one
    return value;
  };

  switch (call->builtin) {
  case Builtin::BytesNew:
  case Builtin::ArrayBufferNew: {
    llvm::Value *length = emitExpr(call->args[0]);
    llvm::Value *made;
    if (call->builtin == Builtin::ArrayBufferNew)
      made = builder_.CreateCall(fn("tz_abuf_new", ptr, {i64}), {length}, "buffer");
    else if (type->isBuffer())
      made = builder_.CreateCall(fn("tz_buffer_alloc", ptr, {i64, i32}),
                                 {length, builder_.getInt32(call->args.size() < 2 ||
                                                            cast<BoolLitExpr>(call->args[1])->value)},
                                 "buffer");
    else
      made = builder_.CreateCall(fn("tz_bytes_new", ptr, {i32, i64}), {kindOf(type), length}, "bytes");
    emitThrowCheckIf(throws);
    return made;
  }
  case Builtin::BytesFromArray: {
    Expr *source = call->args[0];
    llvm::Value *made;
    if (source->type->isIterator()) {
      // The values first, in an array.
      const Type *element = source->type->yielded();
      llvm::Value *values = builder_.CreateCall(
          rt_.arrNew, {typeDescriptor(element), builder_.getInt64(0), builder_.getInt64(0)}, "values");
      Held held = hold(values, Type::array(element), true);
      emitIteratorSpread(values, source, element);
      values = take(held);
      made = builder_.CreateCall(fn("tz_bytes_from_array", ptr, {i32, ptr}), {kindOf(type), values},
                                 "bytes");
      releaseValue(values, Type::array(element));
      return made;
    }
    Operand array = emitOperand(source, /*canBorrow=*/true);
    made = builder_.CreateCall(fn("tz_bytes_from_array", ptr, {i32, ptr}),
                               {kindOf(type), array.value}, "bytes");
    dropOperand(array);
    return made;
  }
  case Builtin::BytesFromBytes: {
    Operand source = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *made = builder_.CreateCall(fn("tz_bytes_from_bytes", ptr, {i32, ptr}),
                                            {kindOf(type), source.value}, "bytes");
    dropOperand(source);
    return made;
  }
  case Builtin::BytesOf: {
    llvm::Value *made = builder_.CreateCall(
        fn("tz_bytes_new_unset", ptr, {i32, i64}),
        {kindOf(type), builder_.getInt64(call->args.size())}, "bytes");
    if (call->args.empty())
      return made;
    // Nothing else reaches it while the values are evaluated; one that
    // throws drops it, with the elements stored so far.
    Held held = hold(made, type, llvm::any_of(call->args, [&](Expr *e) { return exprThrows(e); }));
    for (size_t i = 0; i < call->args.size(); ++i)
      storeElement(bytesData(made), type, builder_.getInt64(i), emitExpr(call->args[i]));
    return take(held);
  }
  case Builtin::BytesView:
  case Builtin::DataViewNew: {
    Operand buffer = emitOperand(call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                                             cannotAssign));
    holdOperand(buffer, llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                     [&](Expr *e) { return exprThrows(e); }));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 2, values);
    takeOperand(buffer);
    llvm::Value *made;
    if (call->builtin == Builtin::DataViewNew)
      made = builder_.CreateCall(fn("tz_dataview_new", ptr, {ptr, i64, i64, i32}),
                                 {buffer.value, values[0], values[1], has}, "view");
    else if (type->isBuffer())
      made = builder_.CreateCall(fn("tz_buffer_view", ptr, {ptr, i64, i64, i32}),
                                 {buffer.value, values[0], values[1], has}, "view");
    else
      made = builder_.CreateCall(fn("tz_bytes_view", ptr, {i32, ptr, i64, i64, i32}),
                                 {kindOf(type), buffer.value, values[0], values[1], has}, "view");
    dropOperand(buffer);
    emitThrowCheck();
    return made;
  }
  case Builtin::BytesByteLength: {
    Operand self = emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *length = bytesLength(self.value);
    auto sizeOf = [](const Type *t) { return t->isTypedArray() ? t->elementSize() : 1u; };
    llvm::Value *size = builder_.getInt64(sizeOf(self.type));
    if (self.type->isUnion()) {
      // Of the member the value is, by its class number, where elements of
      // more than a byte are among them.
      llvm::Value *id = nullptr;
      for (const Type *m : self.type->members()) {
        if (sizeOf(m) == 1)
          continue;
        if (!id)
          id = objectClassId(self.value);
        size = builder_.CreateSelect(builder_.CreateICmpEQ(id, builder_.getInt32(plainTypeId(m))),
                                     builder_.getInt64(sizeOf(m)), size);
      }
    }
    dropOperand(self);
    return builder_.CreateTrunc(builder_.CreateMul(length, size), i32);
  }
  case Builtin::BytesByteOffset: {
    Operand self = emitOperand(member->object, /*canBorrow=*/true);
    if (!bytesTy_)
      bytesTy_ = bytesStruct(ctx_);
    llvm::Value *offset =
        builder_.CreateLoad(i32, builder_.CreateStructGEP(bytesTy_, self.value, kOffset), "offset");
    dropOperand(self);
    return offset;
  }
  case Builtin::BytesBuffer: {
    Operand self = emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *buffer = builder_.CreateCall(fn("tz_bytes_buffer", ptr, {ptr}), {self.value});
    dropOperand(self);
    return buffer;
  }
  case Builtin::BytesAt: {
    Operand self = receiver(0);
    llvm::Value *at = emitExpr(call->args[0]);
    takeOperand(self);
    llvm::Value *length = bytesLength(self.value);
    at = builder_.CreateSelect(builder_.CreateICmpSLT(at, builder_.getInt64(0)),
                               builder_.CreateAdd(at, length), at);
    const Type *bytesType = self.type;
    auto read = [&] { return loadElement(bytesData(self.value), bytesType, at); };
    llvm::Value *result = call->mayBeOutOfRange
                              ? emitCheckedRead(builder_.CreateICmpULT(at, length), type, read)
                              : read();
    dropOperand(self);
    return result;
  }
  case Builtin::BytesIndexOf: {
    Operand self = receiver(1);
    llvm::Value *value = emitExpr(call->args[1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(2, 1, values);
    takeOperand(self);
    llvm::Value *mode = builder_.getInt32(uint32_t(constantOf(call->args[0])));
    llvm::Value *found = builder_.CreateCall(fn("tz_bytes_index_of", i32, {ptr, f64, i64, i32, i32}),
                                             {self.value, value, values[0], has, mode});
    dropOperand(self);
    return constantOf(call->args[0]) == 2 ? builder_.CreateICmpNE(found, builder_.getInt32(0)) : found;
  }
  case Builtin::BytesJoin: {
    Operand self = receiver(0);
    llvm::Value *text;
    if (call->args.empty()) {
      takeOperand(self);
      llvm::Value *none = llvm::ConstantPointerNull::get(builder_.getPtrTy());
      text = callReturningString(fn("tz_bytes_join", voidTy, {ptr, ptr, ptr, i64, i32}),
                                 {self.value, none, builder_.getInt64(0), builder_.getInt32(0)});
    } else {
      Operand separator = emitOperand(call->args[0], /*canBorrow=*/true);
      takeOperand(self);
      text = callReturningString(fn("tz_bytes_join", voidTy, {ptr, ptr, ptr, i64, i32}),
                                 {self.value, stringObj(separator.value),
                                  stringMeta(separator.value), builder_.getInt32(1)});
      dropOperand(separator);
    }
    dropOperand(self);
    return text;
  }
  case Builtin::BytesReverse:
  case Builtin::BytesSort: {
    Operand self = receiver(0);
    takeOperand(self);
    builder_.CreateCall(fn(call->builtin == Builtin::BytesSort ? "tz_bytes_sort" : "tz_bytes_reverse",
                           voidTy, {ptr}),
                        {self.value});
    return returnSelf(self);
  }
  case Builtin::BytesFill: {
    Operand self = receiver(0);
    llvm::Value *value = emitExpr(call->args[0]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 2, values);
    takeOperand(self);
    builder_.CreateCall(fn("tz_bytes_fill", voidTy, {ptr, f64, i64, i64, i32}),
                        {self.value, value, values[0], values[1], has});
    return returnSelf(self);
  }
  case Builtin::BytesSet: {
    Operand self = receiver(0);
    Expr *source = call->args[0];
    Operand from = emitOperand(source, call->args.size() < 2 || cannotAssign(call->args[1]));
    holdOperand(from, call->args.size() > 1 && exprThrows(call->args[1]));
    llvm::Value *offset = call->args.size() > 1 ? emitExpr(call->args[1]) : builder_.getInt64(0);
    takeOperand(self);
    takeOperand(from);
    builder_.CreateCall(fn(source->type->isArray() ? "tz_bytes_set_array" : "tz_bytes_set_bytes",
                           voidTy, {ptr, ptr, i64}),
                        {self.value, from.value, offset});
    dropOperand(from);
    dropOperand(self);
    emitThrowCheck(); // RangeError: offset is out of bounds
    return nullptr;
  }
  case Builtin::BytesSubarray:
  case Builtin::BytesSlice:
  case Builtin::ArrayBufferSlice: {
    Operand self = receiver(0);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(0, 2, values);
    takeOperand(self);
    llvm::StringRef name = call->builtin == Builtin::BytesSubarray ? "tz_bytes_subarray"
                           : call->builtin == Builtin::BytesSlice  ? "tz_bytes_slice"
                                                                   : "tz_abuf_slice";
    llvm::Value *made = builder_.CreateCall(fn(name, ptr, {ptr, i64, i64, i32}),
                                            {self.value, values[0], values[1], has});
    dropOperand(self);
    return made;
  }
  case Builtin::BytesCopyWithin: {
    Operand self = receiver(0);
    std::vector<llvm::Value *> values;
    for (size_t i = 0; i < 2; ++i)
      values.push_back(emitExpr(call->args[i]));
    llvm::Value *end = call->args.size() > 2 ? emitExpr(call->args[2]) : builder_.getInt64(0);
    takeOperand(self);
    builder_.CreateCall(fn("tz_bytes_copy_within", voidTy, {ptr, i64, i64, i64, i32}),
                        {self.value, values[0], values[1], end,
                         builder_.getInt32(call->args.size() > 2)});
    return returnSelf(self);
  }
  case Builtin::BytesToReversed:
  case Builtin::BytesToSorted: {
    Operand self = receiver(0);
    takeOperand(self);
    llvm::Value *made = builder_.CreateCall(
        fn(call->builtin == Builtin::BytesToSorted ? "tz_bytes_to_sorted" : "tz_bytes_to_reversed",
           ptr, {ptr}),
        {self.value});
    dropOperand(self);
    return made;
  }
  case Builtin::BytesWith: {
    Operand self = receiver(0);
    llvm::Value *index = emitExpr(call->args[0]);
    llvm::Value *value = emitExpr(call->args[1]);
    takeOperand(self);
    llvm::Value *made = builder_.CreateCall(fn("tz_bytes_with", ptr, {ptr, i64, f64}),
                                            {self.value, index, value});
    dropOperand(self);
    emitThrowCheck(); // RangeError: Invalid typed array index
    return made;
  }
  case Builtin::ArrayBufferIsView: {
    emitDiscarded(call->args[0]);
    return builder_.getInt1(call->args[0]->type->nonOptional()->isByteView());
  }
  case Builtin::DataViewGet:
  case Builtin::DataViewSet:
  case Builtin::BufferRead:
  case Builtin::BufferWriteNumber: {
    // args: the kind, then for a DataView the offset, the value to set and
    // littleEndian?; for a Buffer whether it is big-endian, the value to
    // write and the offset? (0 when left out).
    bool dataView = call->builtin == Builtin::DataViewGet || call->builtin == Builtin::DataViewSet;
    bool write = call->builtin == Builtin::DataViewSet || call->builtin == Builtin::BufferWriteNumber;
    const Type *kindType = Type::bytesType(Bytes(constantOf(call->args[0])));
    Bytes kind = kindType->bytes();
    unsigned size = kindType->elementSize();
    Operand self = receiver(1);
    llvm::Value *offset = nullptr, *value = nullptr, *little = nullptr;
    if (dataView) {
      offset = emitExpr(call->args[1]);
      if (write)
        value = emitExpr(call->args[2]);
      size_t at = write ? 3 : 2;
      little = call->args.size() > at ? emitExpr(call->args[at]) : builder_.getFalse();
    } else {
      little = builder_.getInt1(!cast<BoolLitExpr>(call->args[1])->value);
      size_t at = 2;
      if (write)
        value = emitExpr(call->args[at++]);
      offset = call->args.size() > at ? emitExpr(call->args[at]) : builder_.getInt64(0);
    }
    takeOperand(self);
    llvm::Value *length = bytesLength(self.value);
    if (call->mayBeOutOfRange) {
      // 0 <= offset and offset + size <= length, as unsigned numbers.
      llvm::Value *ok = builder_.CreateAnd(
          builder_.CreateICmpULT(offset, length),
          builder_.CreateICmpULE(builder_.getInt64(size), builder_.CreateSub(length, offset)));
      llvm::BasicBlock *badBlock = newBlock("bytes.out.of.range");
      llvm::BasicBlock *okBlock = newBlock("bytes.in.range");
      builder_.CreateCondBr(ok, okBlock, badBlock, llvm::MDBuilder(ctx_).createLikelyBranchWeights());
      switchTo(badBlock);
      if (dataView)
        builder_.CreateCall(fn("tz_dataview_range_error", voidTy, {}));
      else
        builder_.CreateCall(fn("tz_buffer_bounds_error", voidTy, {i64, i64, i64}),
                            {offset, builder_.getInt64(size), length});
      Operand released = self;
      dropOperand(released);
      emitRuntimeThrow();
      switchTo(okBlock);
    }
    llvm::Value *address = builder_.CreateInBoundsGEP(builder_.getInt8Ty(), bytesData(self.value), offset);
    llvm::Type *bits = builder_.getIntNTy(size * 8);
    auto swapped = [&](llvm::Value *raw) -> llvm::Value * {
      if (size == 1)
        return raw;
      llvm::Value *swap = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, raw);
      // The bytes lie little-endian in memory.
      if (auto *constant = dyn_cast<llvm::ConstantInt>(little))
        return constant->isOne() ? raw : swap;
      return builder_.CreateSelect(little, raw, swap);
    };
    llvm::Value *result;
    if (!write) {
      llvm::LoadInst *raw = builder_.CreateAlignedLoad(bits, address, llvm::Align(1));
      raw->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/false));
      llvm::Value *v = swapped(raw);
      switch (kind) {
      case Bytes::Float32:
        result = builder_.CreateFPExt(builder_.CreateBitCast(v, builder_.getFloatTy()), f64);
        break;
      case Bytes::Float64:
        result = builder_.CreateBitCast(v, f64);
        break;
      case Bytes::Float16:
        result = builder_.CreateCall(fn("tz_f16_to_f64", f64, {i32}), {builder_.CreateZExt(v, i32)});
        break;
      case Bytes::Uint32:
        result = builder_.CreateZExt(v, i64);
        break;
      case Bytes::Int32:
        result = v;
        break;
      default:
        result = isSignedKind(kind) ? builder_.CreateSExt(v, i32) : builder_.CreateZExt(v, i32);
        break;
      }
    } else {
      llvm::Value *v;
      switch (kind) {
      case Bytes::Float32:
        v = builder_.CreateBitCast(builder_.CreateFPTrunc(value, builder_.getFloatTy()), bits);
        break;
      case Bytes::Float64:
        v = builder_.CreateBitCast(value, bits);
        break;
      case Bytes::Float16:
        v = builder_.CreateTrunc(builder_.CreateCall(fn("tz_f16_from_f64", i32, {f64}), {value}), bits);
        break;
      default:
        v = value->getType() == bits ? value : builder_.CreateTrunc(value, bits);
        break;
      }
      llvm::StoreInst *store = builder_.CreateAlignedStore(swapped(v), address, llvm::Align(1));
      store->setMetadata(llvm::LLVMContext::MD_tbaa, bytesTbaa(/*header=*/false));
      // A Buffer's write gives the offset after what it wrote.
      result = dataView ? nullptr
                        : builder_.CreateTrunc(builder_.CreateAdd(offset, builder_.getInt64(size)), i32);
    }
    dropOperand(self);
    return result;
  }
  case Builtin::BufferReadVar:
  case Builtin::BufferWriteVar: {
    bool write = call->builtin == Builtin::BufferWriteVar;
    Operand self = receiver(2);
    std::vector<llvm::Value *> values;
    for (size_t i = 2; i < call->args.size(); ++i)
      values.push_back(emitExpr(call->args[i]));
    takeOperand(self);
    uint32_t flags = uint32_t(cast<BoolLitExpr>(call->args[0])->value) |
                     uint32_t(cast<BoolLitExpr>(call->args[1])->value) << 1;
    llvm::Value *result;
    if (write) {
      result = builder_.CreateCall(fn("tz_buffer_write_var", i64, {ptr, i64, i64, i64, i32}),
                                   {self.value, values[0], values[1], values[2], builder_.getInt32(flags)});
      result = builder_.CreateTrunc(result, i32);
    } else {
      result = builder_.CreateCall(fn("tz_buffer_read_var", i64, {ptr, i64, i64, i32}),
                                   {self.value, values[0], values[1], builder_.getInt32(flags)});
    }
    dropOperand(self);
    emitThrowCheck();
    return result;
  }
  case Builtin::BufferFromString: {
    Operand text = emitOperand(call->args[0], cannotAssign(call->args[1]));
    holdOperand(text, throws || exprThrows(call->args[1]));
    llvm::Value *enc = encoding(call->args[1]);
    takeOperand(text);
    llvm::Value *made = builder_.CreateCall(fn("tz_buffer_from_string", ptr, {ptr, i64, i32}),
                                            {stringObj(text.value), stringMeta(text.value), enc},
                                            "buffer");
    dropOperand(text);
    return made;
  }
  case Builtin::BufferAllocFill: {
    llvm::Value *size = emitExpr(call->args[0]);
    Expr *fillExpr = call->args[1];
    Operand fill = emitOperand(fillExpr, cannotAssign(call->args[2]));
    holdOperand(fill, throws || exprThrows(call->args[2]));
    llvm::Value *enc = encoding(call->args[2]);
    takeOperand(fill);
    llvm::Value *made =
        builder_.CreateCall(fn("tz_buffer_alloc", ptr, {i64, i32}), {size, builder_.getInt32(1)}, "buffer");
    llvm::Value *zero = builder_.getInt64(0), *none = builder_.getInt32(0);
    // A buffer that failed is NULL: nothing to fill, and the error goes up.
    llvm::BasicBlock *fillBlock = newBlock("alloc.fill");
    llvm::BasicBlock *endBlock = newBlock("alloc.end");
    builder_.CreateCondBr(builder_.CreateIsNotNull(made), fillBlock, endBlock);
    switchTo(fillBlock);
    if (fillExpr->type->isString())
      builder_.CreateCall(fn("tz_buffer_fill_string", voidTy, {ptr, ptr, i64, i64, i64, i32, i32}),
                          {made, stringObj(fill.value), stringMeta(fill.value), zero, zero, none, enc});
    else if (fillExpr->type->isInt())
      builder_.CreateCall(fn("tz_buffer_fill_byte", voidTy, {ptr, i32, i64, i64, i32}),
                          {made, fill.value, zero, zero, none});
    else
      builder_.CreateCall(fn("tz_buffer_fill_bytes", voidTy, {ptr, ptr, i64, i64, i32}),
                          {made, fill.value, zero, zero, none});
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    dropOperand(fill);
    Held held = hold(made, type, true);
    emitThrowCheck();
    return take(held);
  }
  case Builtin::BufferConcat: {
    Operand list = emitOperand(call->args[0], call->args.size() < 2 || cannotAssign(call->args[1]));
    holdOperand(list, call->args.size() > 1 && exprThrows(call->args[1]));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 1, values);
    takeOperand(list);
    llvm::Value *made = builder_.CreateCall(fn("tz_buffer_concat", ptr, {ptr, i64, i32}),
                                            {list.value, values[0], has}, "buffer");
    dropOperand(list);
    emitThrowCheck();
    return made;
  }
  case Builtin::BufferByteLength: {
    Expr *source = call->args[0];
    if (source->type->isString()) {
      Operand text = emitOperand(source, cannotAssign(call->args[1]));
      holdOperand(text, throws || exprThrows(call->args[1]));
      llvm::Value *enc = encoding(call->args[1]);
      takeOperand(text);
      llvm::Value *length = builder_.CreateCall(fn("tz_buffer_byte_length", i32, {ptr, i64, i32}),
                                                {stringObj(text.value), stringMeta(text.value), enc});
      dropOperand(text);
      return length;
    }
    Operand bytes = emitOperand(source, /*canBorrow=*/true);
    llvm::Value *length = bytesLength(bytes.value);
    dropOperand(bytes);
    unsigned size = source->type->isTypedArray() ? source->type->elementSize() : 1;
    return builder_.CreateTrunc(builder_.CreateMul(length, builder_.getInt64(size)), i32);
  }
  case Builtin::BufferCompare:
  case Builtin::BufferEquals: {
    Expr *first = call->builtin == Builtin::BufferCompare ? call->args[0] : member->object;
    Expr *second = call->builtin == Builtin::BufferCompare ? call->args[1] : call->args[0];
    Operand a = emitOperand(first, cannotAssign(second));
    holdOperand(a, exprThrows(second));
    Operand b = emitOperand(second, /*canBorrow=*/true);
    takeOperand(a);
    llvm::Value *result =
        call->builtin == Builtin::BufferCompare
            ? builder_.CreateCall(fn("tz_bytes_compare", i32, {ptr, ptr}), {a.value, b.value})
            : builder_.CreateICmpNE(
                  builder_.CreateCall(fn("tz_bytes_equal", i32, {ptr, ptr}), {a.value, b.value}),
                  builder_.getInt32(0));
    dropOperand(b);
    dropOperand(a);
    return result;
  }
  case Builtin::BufferCompareTo: {
    Operand self = receiver(0);
    Operand target = emitOperand(call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                                             cannotAssign));
    holdOperand(target, llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                     [&](Expr *e) { return exprThrows(e); }));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 4, values);
    takeOperand(self);
    takeOperand(target);
    llvm::Value *result =
        builder_.CreateCall(fn("tz_buffer_compare_range", i32, {ptr, ptr, i64, i64, i64, i64, i32}),
                            {self.value, target.value, values[0], values[1], values[2], values[3], has});
    dropOperand(target);
    dropOperand(self);
    emitThrowCheck();
    return result;
  }
  case Builtin::BufferIsBuffer: {
    const Type *t = call->args[0]->type;
    if (!t->nonOptional()->isTypedArray() || t->nonOptional()->bytes() != Bytes::Uint8 ||
        t->isOptional()) {
      emitDiscarded(call->args[0]);
      return builder_.getInt1(t->isBuffer());
    }
    // A Uint8Array may be a Buffer.
    Operand bytes = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *is = builder_.CreateCall(fn("tz_bytes_is_buffer", i32, {ptr}), {bytes.value});
    dropOperand(bytes);
    return builder_.CreateICmpNE(is, builder_.getInt32(0));
  }
  case Builtin::BufferIsEncoding: {
    Operand name = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *found = builder_.CreateCall(fn("tz_buffer_encoding", i32, {ptr, i64}),
                                             {stringObj(name.value), stringMeta(name.value)});
    dropOperand(name);
    return builder_.CreateICmpSGE(found, builder_.getInt32(0));
  }
  case Builtin::BufferToString: {
    Operand self = receiver(0);
    llvm::Value *enc = encoding(call->args[0]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 2, values);
    takeOperand(self);
    llvm::Value *text = callReturningString(
        fn("tz_buffer_to_string", voidTy, {ptr, ptr, i32, i64, i64, i32}),
        {self.value, enc, values[0], values[1], has});
    dropOperand(self);
    return text;
  }
  case Builtin::BufferWrite: {
    Operand self = receiver(0);
    Operand text = emitOperand(call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                                           cannotAssign));
    holdOperand(text, throws || llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                             [&](Expr *e) { return exprThrows(e); }));
    llvm::Value *enc = encoding(call->args[1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(2, 2, values);
    takeOperand(self);
    takeOperand(text);
    llvm::Value *written = builder_.CreateCall(
        fn("tz_buffer_write_string", i32, {ptr, ptr, i64, i64, i64, i32, i32}),
        {self.value, stringObj(text.value), stringMeta(text.value), values[0], values[1], has, enc});
    dropOperand(text);
    dropOperand(self);
    emitThrowCheck();
    return written;
  }
  case Builtin::BufferIndexOf:
  case Builtin::BufferFill: {
    bool fill = call->builtin == Builtin::BufferFill;
    size_t valueAt = fill ? 0 : 1;
    Expr *valueExpr = call->args[valueAt];
    Operand self = receiver(valueAt);
    Operand value = emitOperand(valueExpr, llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(valueAt + 1),
                                                        cannotAssign));
    holdOperand(value, throws || llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(valueAt + 1),
                                              [&](Expr *e) { return exprThrows(e); }));
    llvm::Value *enc = encoding(call->args[valueAt + 1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(valueAt + 2, fill ? 2 : 1, values);
    takeOperand(self);
    takeOperand(value);
    llvm::Value *result = nullptr;
    if (fill) {
      if (valueExpr->type->isString())
        builder_.CreateCall(fn("tz_buffer_fill_string", voidTy, {ptr, ptr, i64, i64, i64, i32, i32}),
                            {self.value, stringObj(value.value), stringMeta(value.value), values[0],
                             values[1], has, enc});
      else if (valueExpr->type->isInt())
        builder_.CreateCall(fn("tz_buffer_fill_byte", voidTy, {ptr, i32, i64, i64, i32}),
                            {self.value, value.value, values[0], values[1], has});
      else
        builder_.CreateCall(fn("tz_buffer_fill_bytes", voidTy, {ptr, ptr, i64, i64, i32}),
                            {self.value, value.value, values[0], values[1], has});
      dropOperand(value);
      Held held = hold(returnSelf(self), self.type, true);
      emitThrowCheck();
      return take(held);
    }
    llvm::Value *mode = builder_.getInt32(uint32_t(constantOf(call->args[0])));
    if (valueExpr->type->isString())
      result = builder_.CreateCall(
          fn("tz_buffer_index_of_string", i32, {ptr, ptr, i64, i64, i32, i32, i32}),
          {self.value, stringObj(value.value), stringMeta(value.value), values[0], has, enc, mode});
    else if (valueExpr->type->isInt())
      result = builder_.CreateCall(fn("tz_buffer_index_of_byte", i32, {ptr, i32, i64, i32, i32}),
                                   {self.value, value.value, values[0], has, mode});
    else
      result = builder_.CreateCall(fn("tz_buffer_index_of_bytes", i32, {ptr, ptr, i64, i32, i32}),
                                   {self.value, value.value, values[0], has, mode});
    dropOperand(value);
    dropOperand(self);
    return constantOf(call->args[0]) == 2 ? builder_.CreateICmpNE(result, builder_.getInt32(0)) : result;
  }
  case Builtin::BufferCopy: {
    Operand self = receiver(0);
    Operand target = emitOperand(call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                                             cannotAssign));
    holdOperand(target, llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                     [&](Expr *e) { return exprThrows(e); }));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 3, values);
    takeOperand(self);
    takeOperand(target);
    llvm::Value *copied = builder_.CreateCall(
        fn("tz_buffer_copy", i32, {ptr, ptr, i64, i64, i64, i32}),
        {self.value, target.value, values[0], values[1], values[2], has});
    dropOperand(target);
    dropOperand(self);
    emitThrowCheck();
    return copied;
  }
  case Builtin::BufferSwap: {
    Operand self = receiver(0);
    takeOperand(self);
    builder_.CreateCall(fn("tz_buffer_swap", voidTy, {ptr, i32}),
                        {self.value, builder_.getInt32(uint32_t(constantOf(call->args[0])))});
    Held held = hold(returnSelf(self), self.type, true);
    emitThrowCheck();
    return take(held);
  }
  case Builtin::BufferToJSON: {
    Operand self = receiver(0);
    takeOperand(self);
    llvm::Value *object = allocObject(type, {0, 1});
    builder_.CreateStore(stringLiteral(u"Buffer"), fieldAddress(object, type, "type"));
    llvm::Value *data = builder_.CreateCall(fn("tz_bytes_to_array", ptr, {ptr, ptr}),
                                            {self.value, typeDescriptor(Type::intTy())});
    builder_.CreateStore(data, fieldAddress(object, type, "data"));
    dropOperand(self);
    return object;
  }
  case Builtin::TextEncoderNew:
    return builder_.CreateCall(fn("tz_text_encoder_new", ptr, {}), {}, "encoder");
  case Builtin::TextEncode: {
    Operand self = receiver(0);
    if (call->args.empty()) {
      llvm::Value *empty = stringLiteral(u"");
      takeOperand(self);
      dropOperand(self);
      return builder_.CreateCall(fn("tz_text_encode", ptr, {ptr, i64}),
                                 {stringObj(empty), stringMeta(empty)}, "bytes");
    }
    Operand text = emitOperand(call->args[0], /*canBorrow=*/true);
    takeOperand(self);
    llvm::Value *made = builder_.CreateCall(fn("tz_text_encode", ptr, {ptr, i64}),
                                            {stringObj(text.value), stringMeta(text.value)}, "bytes");
    dropOperand(text);
    dropOperand(self);
    return made;
  }
  case Builtin::TextEncodeInto: {
    Operand self = receiver(0);
    Operand text = emitOperand(call->args[0], cannotAssign(call->args[1]));
    holdOperand(text, throws || exprThrows(call->args[1]));
    Operand target = emitOperand(call->args[1], /*canBorrow=*/true);
    takeOperand(self);
    takeOperand(text);
    auto *pairTy = llvm::ArrayType::get(i32, 2);
    llvm::AllocaInst *out = createStackSlot(pairTy, "encoded");
    builder_.CreateCall(fn("tz_text_encode_into", voidTy, {ptr, i64, ptr, ptr}),
                        {stringObj(text.value), stringMeta(text.value), target.value, out});
    dropOperand(target);
    dropOperand(text);
    dropOperand(self);
    llvm::Value *object = allocObject(type, {0, 1});
    builder_.CreateStore(builder_.CreateLoad(i32, builder_.CreateConstInBoundsGEP2_32(pairTy, out, 0, 0)),
                         fieldAddress(object, type, "read"));
    builder_.CreateStore(builder_.CreateLoad(i32, builder_.CreateConstInBoundsGEP2_32(pairTy, out, 0, 1)),
                         fieldAddress(object, type, "written"));
    return object;
  }
  case Builtin::TextDecoderNew: {
    // The options first, as JavaScript reads them after the label.
    bool resolved = constantOf(call->args[0]) != 0;
    Operand label{nullptr, false, nullptr};
    if (!resolved) {
      label = emitOperand(call->args[1], call->args.size() < 3 || cannotAssign(call->args[2]));
      holdOperand(label, call->args.size() > 2 && exprThrows(call->args[2]));
    }
    llvm::Value *fatal = builder_.getInt32(0), *ignore = builder_.getInt32(0);
    if (call->args.size() > 2) {
      Operand options = emitOperand(call->args[2], /*canBorrow=*/true);
      const Type *optionsType = options.type;
      auto flag = [&](llvm::StringRef name) -> llvm::Value * {
        const Type *t = optionsType->field(name)->type;
        llvm::Value *loaded = builder_.CreateLoad(lower(t), fieldAddress(options.value, optionsType, name));
        llvm::Value *value = builder_.CreateAnd(optionalHas(loaded, t), optionalValue(loaded, t));
        return builder_.CreateZExt(value, i32);
      };
      fatal = flag("fatal");
      ignore = flag("ignoreBOM");
      dropOperand(options);
    }
    if (resolved) {
      std::string name;
      for (char16_t c : cast<StringLitExpr>(call->args[1])->value)
        name += char(c);
      bool multi = tz_encoding_multi_byte(name.c_str());
      int which = name == "utf-8"      ? kTextUtf8
                  : name == "utf-16le" ? kTextUtf16le
                  : name == "utf-16be" ? kTextUtf16be
                  : multi              ? kTextCjk
                                       : kTextSingleByte;
      llvm::Value *table = llvm::ConstantPointerNull::get(builder_.getPtrTy());
      if (which == kTextSingleByte)
        table = encodingTable(name);
      else if (which == kTextCjk)
        table = cjkEncodingOf(*module_, name);
      return builder_.CreateCall(fn("tz_text_decoder_make", ptr, {i32, ptr, i32, i32}),
                                 {builder_.getInt32(uint32_t(which)), table, fatal, ignore},
                                 "decoder");
    }
    // A label known only now: the program holds every table.
    std::vector<llvm::Constant *> tables, cjk;
    for (const EncodingTable &t : kEncodingTables)
      tables.push_back(encodingTable(t.name));
    for (const char *name : kCjkEncodings)
      cjk.push_back(cjkEncodingOf(*module_, name));
    auto list = [&](std::vector<llvm::Constant *> &items, const char *name) {
      auto *type = llvm::ArrayType::get(builder_.getPtrTy(), items.size());
      return new llvm::GlobalVariable(*module_, type, /*isConstant=*/true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      llvm::ConstantArray::get(type, items), name);
    };
    llvm::Value *all = list(tables, "tz.encodings"), *allCjk = list(cjk, "tz.encodings.cjk");
    takeOperand(label);
    llvm::Value *made = builder_.CreateCall(
        fn("tz_text_decoder_new", ptr, {ptr, i64, ptr, i32, ptr, i32, i32, i32}),
        {stringObj(label.value), stringMeta(label.value), all,
         builder_.getInt32(uint32_t(tables.size())), allCjk,
         builder_.getInt32(uint32_t(cjk.size())), fatal, ignore},
        "decoder");
    dropOperand(label);
    emitThrowCheck(); // RangeError: the encoding is not supported
    return made;
  }
  case Builtin::TextDecode: {
    Operand self = receiver(0);
    llvm::Value *data = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    llvm::Value *length = builder_.getInt64(0);
    Operand input{nullptr, false, nullptr};
    if (!call->args.empty() && !call->args[0]->type->isUndefined()) {
      input = emitOperand(call->args[0], /*canBorrow=*/true);
      data = bytesData(input.value);
      length = bytesLength(input.value);
      if (input.type->isTypedArray())
        length = builder_.CreateMul(length, builder_.getInt64(input.type->elementSize()));
    }
    // `{ stream: true }`: the text goes on in the next call.
    llvm::Value *stream = builder_.getInt32(0);
    if (call->args.size() > 1) {
      Operand options = emitOperand(call->args[1], /*canBorrow=*/true);
      const Type *t = options.type->field("stream")->type;
      llvm::Value *loaded =
          builder_.CreateLoad(lower(t), fieldAddress(options.value, options.type, "stream"));
      llvm::Value *given = builder_.CreateAnd(optionalHas(loaded, t), optionalValue(loaded, t));
      stream = builder_.CreateZExt(given, i32);
      dropOperand(options);
    }
    takeOperand(self);
    llvm::Value *text = callReturningString(fn("tz_text_decode", voidTy, {ptr, ptr, ptr, i64, i32}),
                                            {self.value, data, length, stream});
    if (input.value)
      dropOperand(input);
    dropOperand(self);
    emitThrowCheck(); // TypeError of a fatal decoder
    return text;
  }
  case Builtin::TextDecoderInfo: {
    Operand self = receiver(0);
    takeOperand(self);
    int64_t which = constantOf(call->args[0]);
    llvm::Value *result;
    if (which == 0)
      result = callReturningString(fn("tz_text_decoder_encoding", voidTy, {ptr, ptr}), {self.value});
    else
      result = builder_.CreateICmpNE(
          builder_.CreateCall(fn("tz_text_decoder_flag", i32, {ptr, i32}),
                              {self.value, builder_.getInt32(uint32_t(which - 1))}),
          builder_.getInt32(0));
    dropOperand(self);
    return result;
  }
  case Builtin::EmbedBytes: {
    // One constant Uint8Array for each file, over a constant ArrayBuffer and
    // bytes the executable holds; counted, but never down to zero. The bytes
    // are writable, so a program may change them, as it may change what any
    // embedBytes of the file gives.
    const std::u16string &path = cast<StringLitExpr>(call->args[1])->value;
    llvm::Constant *&global = embeddedBytes_[path];
    if (!global) {
      const std::u16string &units = cast<StringLitExpr>(call->args[0])->value;
      std::vector<uint8_t> bytes;
      bytes.reserve(units.size());
      for (char16_t unit : units)
        bytes.push_back(uint8_t(unit));
      llvm::Constant *init = llvm::ConstantDataArray::get(ctx_, bytes);
      auto *data = new llvm::GlobalVariable(*module_, init->getType(), /*isConstant=*/false,
                                            llvm::GlobalValue::PrivateLinkage, init, "tz.embedded.data");
      data->setAlignment(llvm::Align(16));
      auto *layoutsTy = llvm::ArrayType::get(layoutDescTy_, 16);
      auto *layouts = cast<llvm::GlobalVariable>(
          module_->getOrInsertGlobal("tz_bytes_layouts", layoutsTy));
      auto layout = [&](Bytes kind) {
        return llvm::ConstantExpr::getInBoundsGetElementPtr(
            layoutsTy, layouts,
            llvm::ArrayRef<llvm::Constant *>{builder_.getInt32(0), builder_.getInt32(uint32_t(kind))});
      };
      // The count of a constant never reaches zero (as for regexes).
      llvm::Constant *count = builder_.getInt64(uint64_t(1) << 62);
      llvm::Constant *length = builder_.getInt32(uint32_t(bytes.size()));
      auto *abufTy = llvm::StructType::get(ctx_, {i64, ptr, ptr, i32, i32});
      auto *abuf = new llvm::GlobalVariable(
          *module_, abufTy, /*isConstant=*/false, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantStruct::get(abufTy, {count, layout(Bytes::ArrayBuffer), data, length,
                                             builder_.getInt32(1)}), // TZ_ABUF_STATIC
          "tz.embedded.buffer");
      if (!bytesTy_)
        bytesTy_ = bytesStruct(ctx_);
      global = new llvm::GlobalVariable(
          *module_, bytesTy_, /*isConstant=*/false, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantStruct::get(bytesTy_, {count, layout(Bytes::Uint8), data, length,
                                               builder_.getInt32(0), abuf}),
          "tz.embedded");
    }
    return global;
  }
  default:
    llvm_unreachable("not a built-in of binary data");
  }
}

} // namespace tz
