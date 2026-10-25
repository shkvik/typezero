#include "compiler/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
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
  for (char &c : symbol) {
    if (c == '-') {
      c = '_';
    }
  }

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
    if (auto *conv = dyn_cast<CastExpr>(length); conv && conv->isImplicit()) {
      length = conv->operand;
    }
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

namespace {

llvm::StructType *bytesStruct(llvm::LLVMContext &ctx) {
  llvm::Type *ptr = llvm::PointerType::getUnqual(ctx);
  llvm::Type *i32 = llvm::Type::getInt32Ty(ctx), *i64 = llvm::Type::getInt64Ty(ctx);

  return llvm::StructType::get(ctx, {i64, ptr, ptr, i32, i32, ptr});
}

bool isSignedKind(Bytes kind) {
  return kind == Bytes::Int8 || kind == Bytes::Int16 || kind == Bytes::Int32;
}

} // namespace

BytesCallLowerer::BytesCallLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *BytesCallLowerer::emitBytesCall(CallExpr *call) {
  llvm::Type *voidTy = codegen_.builder_.getVoidTy(), *i32 = codegen_.builder_.getInt32Ty(),
             *i64 = codegen_.builder_.getInt64Ty(), *f64 = codegen_.builder_.getDoubleTy(),
             *ptr = codegen_.builder_.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Type *type = call->type;
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return codegen_.processLowerer().processFunction(name, ret, params);
  };
  auto kindOf = [&](const Type *t) { return codegen_.builder_.getInt32(uint32_t(t->bytes())); };
  bool throws = bytesMayThrow(call);
  // The receiver of a method, held while the arguments from `from` on are
  // evaluated if they (or an encoding among them) may throw; takeOperand
  // once they are.
  auto receiver = [&](size_t from) {
    auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
    CodeGen::Operand self = codegen_.refCounts().emitOperand(member->object, llvm::all_of(rest, AssignLowerer::cannotAssign));
    codegen_.throwLowerer().holdOperand(self, throws || llvm::any_of(rest, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));

    return self;
  };
  // A method that gives the receiver back hands over its reference.
  auto returnSelf = [&](CodeGen::Operand &self) {
    if (!self.owned) {
      codegen_.refCounts().retainValue(self.value, self.type);
    }
    self.owned = false;

    return self.value;
  };
  // Optional positions from args[from] on: their values (0 when left out)
  // and a mask of those given, from bit 0 on.
  auto positions = [&](size_t from, size_t count, std::vector<llvm::Value *> &values) {
    uint32_t has = 0;
    for (size_t k = 0; k < count; ++k) {
      if (from + k < call->args.size()) {
        values.push_back(codegen_.exprLowerer().emitExpr(call->args[from + k]));
        has |= 1u << k;
      } else {
        values.push_back(codegen_.builder_.getInt64(0));
      }
    }

    return codegen_.builder_.getInt32(has);
  };
  // An encoding: a constant Sema found, or a string checked now.
  auto encoding = [&](Expr *arg) -> llvm::Value * {
    if (auto *lit = dyn_cast<IntLitExpr>(arg)) {
      return codegen_.builder_.getInt32(uint32_t(lit->value));
    }
    CodeGen::Operand name = codegen_.refCounts().emitOperand(arg, /*canBorrow=*/true);
    llvm::Value *value = codegen_.builder_.CreateCall(fn("tz_buffer_encoding_checked", i32, {ptr, i64}),
                                             {codegen_.stringLowerer().stringObj(name.value), codegen_.stringLowerer().stringMeta(name.value)});
    codegen_.refCounts().dropOperand(name);
    codegen_.throwLowerer().emitThrowCheck(); // an unknown one

    return value;
  };

  switch (call->builtin) {
  case Builtin::BytesNew:
  case Builtin::ArrayBufferNew: {
    llvm::Value *length = codegen_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *made;
    if (call->builtin == Builtin::ArrayBufferNew) {
      made = codegen_.builder_.CreateCall(fn("tz_abuf_new", ptr, {i64}), {length}, "buffer");
    } else if (type->isBuffer()) {
      made =
          codegen_.builder_.CreateCall(fn("tz_buffer_alloc", ptr, {i64, i32}),
                              {length, codegen_.builder_.getInt32(call->args.size() < 2 ||
                                                         cast<BoolLitExpr>(call->args[1])->value)},
                              "buffer");
    } else {
      made =
          codegen_.builder_.CreateCall(fn("tz_bytes_new", ptr, {i32, i64}), {kindOf(type), length}, "bytes");
    }
    codegen_.emitThrowCheckIf(throws);
    return made;
  }
  case Builtin::BytesFromArray: {
    Expr *source = call->args[0];
    llvm::Value *made;
    if (source->type->isIterator()) {
      // The values first, in an array.
      const Type *element = source->type->yielded();
      llvm::Value *values = codegen_.builder_.CreateCall(
          codegen_.rt_.arrNew, {codegen_.typeDescriptors().typeDescriptor(element), codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(0)},
          "values");
      CodeGen::Held held = codegen_.throwLowerer().hold(values, Type::array(element), true);
      codegen_.iteratorLowerer().emitIteratorSpread(values, source, element);
      values = codegen_.throwLowerer().take(held);
      made = codegen_.builder_.CreateCall(fn("tz_bytes_from_array", ptr, {i32, ptr}), {kindOf(type), values},
                                 "bytes");
      codegen_.refCounts().releaseValue(values, Type::array(element));
      return made;
    }
    CodeGen::Operand array = codegen_.refCounts().emitOperand(source, /*canBorrow=*/true);
    made = codegen_.builder_.CreateCall(fn("tz_bytes_from_array", ptr, {i32, ptr}),
                               {kindOf(type), array.value}, "bytes");
    codegen_.refCounts().dropOperand(array);
    return made;
  }
  case Builtin::BytesFromBytes: {
    CodeGen::Operand source = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *made = codegen_.builder_.CreateCall(fn("tz_bytes_from_bytes", ptr, {i32, ptr}),
                                            {kindOf(type), source.value}, "bytes");
    codegen_.refCounts().dropOperand(source);
    return made;
  }
  case Builtin::BytesOf: {
    llvm::Value *made =
        codegen_.builder_.CreateCall(fn("tz_bytes_new_unset", ptr, {i32, i64}),
                            {kindOf(type), codegen_.builder_.getInt64(call->args.size())}, "bytes");
    if (call->args.empty()) {
      return made;
    }
    // Nothing else reaches it while the values are evaluated; one that
    // throws drops it, with the elements stored so far.
    CodeGen::Held held = codegen_.throwLowerer().hold(made, type, llvm::any_of(call->args, [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); }));
    for (size_t i = 0; i < call->args.size(); ++i) {
      codegen_.bytesLowerer().storeElement(codegen_.bytesLowerer().bytesData(made), type, codegen_.builder_.getInt64(i), codegen_.exprLowerer().emitExpr(call->args[i]));
    }
    return codegen_.throwLowerer().take(held);
  }
  case Builtin::BytesView:
  case Builtin::DataViewNew: {
    CodeGen::Operand buffer = codegen_.refCounts().emitOperand(
        call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), AssignLowerer::cannotAssign));
    codegen_.throwLowerer().holdOperand(buffer, llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                     [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); }));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 2, values);
    codegen_.throwLowerer().takeOperand(buffer);
    llvm::Value *made;
    if (call->builtin == Builtin::DataViewNew) {
      made = codegen_.builder_.CreateCall(fn("tz_dataview_new", ptr, {ptr, i64, i64, i32}),
                                 {buffer.value, values[0], values[1], has}, "view");
    } else if (type->isBuffer()) {
      made = codegen_.builder_.CreateCall(fn("tz_buffer_view", ptr, {ptr, i64, i64, i32}),
                                 {buffer.value, values[0], values[1], has}, "view");
    } else {
      made = codegen_.builder_.CreateCall(fn("tz_bytes_view", ptr, {i32, ptr, i64, i64, i32}),
                                 {kindOf(type), buffer.value, values[0], values[1], has}, "view");
    }
    codegen_.refCounts().dropOperand(buffer);
    codegen_.throwLowerer().emitThrowCheck();
    return made;
  }
  case Builtin::BytesByteLength: {
    CodeGen::Operand self = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *length = codegen_.bytesLowerer().bytesLength(self.value);
    auto sizeOf = [](const Type *t) { return t->isTypedArray() ? t->elementSize() : 1u; };
    llvm::Value *size = codegen_.builder_.getInt64(sizeOf(self.type));
    if (self.type->isUnion()) {
      // Of the member the value is, by its class number, where elements of
      // more than a byte are among them.
      llvm::Value *id = nullptr;
      for (const Type *m : self.type->members()) {
        if (sizeOf(m) == 1) {
          continue;
        }
        if (!id) {
          id = codegen_.unionLowerer().objectClassId(self.value);
        }
        size = codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpEQ(id, codegen_.builder_.getInt32(codegen_.unionLowerer().plainTypeId(m))),
                                     codegen_.builder_.getInt64(sizeOf(m)), size);
      }
    }
    codegen_.refCounts().dropOperand(self);
    return codegen_.builder_.CreateTrunc(codegen_.builder_.CreateMul(length, size), i32);
  }
  case Builtin::BytesByteOffset: {
    CodeGen::Operand self = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    if (!codegen_.bytesTy_) {
      codegen_.bytesTy_ = bytesStruct(codegen_.ctx_);
    }
    llvm::Value *offset =
        codegen_.builder_.CreateLoad(i32, codegen_.builder_.CreateStructGEP(codegen_.bytesTy_, self.value, kOffset), "offset");
    codegen_.refCounts().dropOperand(self);
    return offset;
  }
  case Builtin::BytesBuffer: {
    CodeGen::Operand self = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *buffer = codegen_.builder_.CreateCall(fn("tz_bytes_buffer", ptr, {ptr}), {self.value});
    codegen_.refCounts().dropOperand(self);
    return buffer;
  }
  case Builtin::BytesAt: {
    CodeGen::Operand self = receiver(0);
    llvm::Value *at = codegen_.exprLowerer().emitExpr(call->args[0]);
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *length = codegen_.bytesLowerer().bytesLength(self.value);
    at = codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpSLT(at, codegen_.builder_.getInt64(0)),
                               codegen_.builder_.CreateAdd(at, length), at);
    const Type *bytesType = self.type;
    auto read = [&] { return codegen_.bytesLowerer().loadElement(codegen_.bytesLowerer().bytesData(self.value), bytesType, at); };
    llvm::Value *result = call->mayBeOutOfRange
                              ? codegen_.arrayLowerer().emitCheckedRead(codegen_.builder_.CreateICmpULT(at, length), type, read)
                              : read();
    codegen_.refCounts().dropOperand(self);
    return result;
  }
  case Builtin::BytesIndexOf: {
    CodeGen::Operand self = receiver(1);
    llvm::Value *value = codegen_.exprLowerer().emitExpr(call->args[1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(2, 1, values);
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *mode = codegen_.builder_.getInt32(uint32_t(constantOf(call->args[0])));
    llvm::Value *found =
        codegen_.builder_.CreateCall(fn("tz_bytes_index_of", i32, {ptr, f64, i64, i32, i32}),
                            {self.value, value, values[0], has, mode});
    codegen_.refCounts().dropOperand(self);
    return constantOf(call->args[0]) == 2 ? codegen_.builder_.CreateICmpNE(found, codegen_.builder_.getInt32(0))
                                          : found;
  }
  case Builtin::BytesJoin: {
    CodeGen::Operand self = receiver(0);
    llvm::Value *text;
    if (call->args.empty()) {
      codegen_.throwLowerer().takeOperand(self);
      llvm::Value *none = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
      text = codegen_.stringLowerer().callReturningString(fn("tz_bytes_join", voidTy, {ptr, ptr, ptr, i64, i32}),
                                 {self.value, none, codegen_.builder_.getInt64(0), codegen_.builder_.getInt32(0)});
    } else {
      CodeGen::Operand separator = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      codegen_.throwLowerer().takeOperand(self);
      text = codegen_.stringLowerer().callReturningString(fn("tz_bytes_join", voidTy, {ptr, ptr, ptr, i64, i32}),
                                 {self.value, codegen_.stringLowerer().stringObj(separator.value),
                                  codegen_.stringLowerer().stringMeta(separator.value), codegen_.builder_.getInt32(1)});
      codegen_.refCounts().dropOperand(separator);
    }
    codegen_.refCounts().dropOperand(self);
    return text;
  }
  case Builtin::BytesReverse:
  case Builtin::BytesSort: {
    CodeGen::Operand self = receiver(0);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.builder_.CreateCall(
        fn(call->builtin == Builtin::BytesSort ? "tz_bytes_sort" : "tz_bytes_reverse", voidTy,
           {ptr}),
        {self.value});
    return returnSelf(self);
  }
  case Builtin::BytesFill: {
    CodeGen::Operand self = receiver(0);
    llvm::Value *value = codegen_.exprLowerer().emitExpr(call->args[0]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 2, values);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.builder_.CreateCall(fn("tz_bytes_fill", voidTy, {ptr, f64, i64, i64, i32}),
                        {self.value, value, values[0], values[1], has});
    return returnSelf(self);
  }
  case Builtin::BytesSet: {
    CodeGen::Operand self = receiver(0);
    Expr *source = call->args[0];
    CodeGen::Operand from = codegen_.refCounts().emitOperand(source, call->args.size() < 2 || AssignLowerer::cannotAssign(call->args[1]));
    codegen_.throwLowerer().holdOperand(from, call->args.size() > 1 && codegen_.mayThrow().exprThrows(call->args[1]));
    llvm::Value *offset = call->args.size() > 1 ? codegen_.exprLowerer().emitExpr(call->args[1]) : codegen_.builder_.getInt64(0);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.throwLowerer().takeOperand(from);
    codegen_.builder_.CreateCall(fn(source->type->isArray() ? "tz_bytes_set_array" : "tz_bytes_set_bytes",
                           voidTy, {ptr, ptr, i64}),
                        {self.value, from.value, offset});
    codegen_.refCounts().dropOperand(from);
    codegen_.refCounts().dropOperand(self);
    codegen_.throwLowerer().emitThrowCheck(); // RangeError: offset is out of bounds
    return nullptr;
  }
  case Builtin::BytesSubarray:
  case Builtin::BytesSlice:
  case Builtin::ArrayBufferSlice: {
    CodeGen::Operand self = receiver(0);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(0, 2, values);
    codegen_.throwLowerer().takeOperand(self);
    llvm::StringRef name = call->builtin == Builtin::BytesSubarray ? "tz_bytes_subarray"
                           : call->builtin == Builtin::BytesSlice  ? "tz_bytes_slice"
                                                                   : "tz_abuf_slice";
    llvm::Value *made = codegen_.builder_.CreateCall(fn(name, ptr, {ptr, i64, i64, i32}),
                                            {self.value, values[0], values[1], has});
    codegen_.refCounts().dropOperand(self);
    return made;
  }
  case Builtin::BytesCopyWithin: {
    CodeGen::Operand self = receiver(0);
    std::vector<llvm::Value *> values;
    for (size_t i = 0; i < 2; ++i) {
      values.push_back(codegen_.exprLowerer().emitExpr(call->args[i]));
    }
    llvm::Value *end = call->args.size() > 2 ? codegen_.exprLowerer().emitExpr(call->args[2]) : codegen_.builder_.getInt64(0);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.builder_.CreateCall(
        fn("tz_bytes_copy_within", voidTy, {ptr, i64, i64, i64, i32}),
        {self.value, values[0], values[1], end, codegen_.builder_.getInt32(call->args.size() > 2)});
    return returnSelf(self);
  }
  case Builtin::BytesToReversed:
  case Builtin::BytesToSorted: {
    CodeGen::Operand self = receiver(0);
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *made = codegen_.builder_.CreateCall(
        fn(call->builtin == Builtin::BytesToSorted ? "tz_bytes_to_sorted" : "tz_bytes_to_reversed",
           ptr, {ptr}),
        {self.value});
    codegen_.refCounts().dropOperand(self);
    return made;
  }
  case Builtin::BytesWith: {
    CodeGen::Operand self = receiver(0);
    llvm::Value *index = codegen_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *value = codegen_.exprLowerer().emitExpr(call->args[1]);
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *made =
        codegen_.builder_.CreateCall(fn("tz_bytes_with", ptr, {ptr, i64, f64}), {self.value, index, value});
    codegen_.refCounts().dropOperand(self);
    codegen_.throwLowerer().emitThrowCheck(); // RangeError: Invalid typed array index
    return made;
  }
  case Builtin::ArrayBufferIsView: {
    codegen_.stmtLowerer().emitDiscarded(call->args[0]);
    return codegen_.builder_.getInt1(call->args[0]->type->nonOptional()->isByteView());
  }
  case Builtin::DataViewGet:
  case Builtin::DataViewSet:
  case Builtin::BufferRead:
  case Builtin::BufferWriteNumber: {
    // args: the kind, then for a DataView the offset, the value to set and
    // littleEndian?; for a Buffer whether it is big-endian, the value to
    // write and the offset? (0 when left out).
    bool dataView = call->builtin == Builtin::DataViewGet || call->builtin == Builtin::DataViewSet;
    bool write =
        call->builtin == Builtin::DataViewSet || call->builtin == Builtin::BufferWriteNumber;
    const Type *kindType = Type::bytesType(Bytes(constantOf(call->args[0])));
    Bytes kind = kindType->bytes();
    unsigned size = kindType->elementSize();
    CodeGen::Operand self = receiver(1);
    llvm::Value *offset = nullptr, *value = nullptr, *little = nullptr;
    if (dataView) {
      offset = codegen_.exprLowerer().emitExpr(call->args[1]);
      if (write) {
        value = codegen_.exprLowerer().emitExpr(call->args[2]);
      }
      size_t at = write ? 3 : 2;
      little = call->args.size() > at ? codegen_.exprLowerer().emitExpr(call->args[at]) : codegen_.builder_.getFalse();
    } else {
      little = codegen_.builder_.getInt1(!cast<BoolLitExpr>(call->args[1])->value);
      size_t at = 2;
      if (write) {
        value = codegen_.exprLowerer().emitExpr(call->args[at++]);
      }
      offset = call->args.size() > at ? codegen_.exprLowerer().emitExpr(call->args[at]) : codegen_.builder_.getInt64(0);
    }
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *length = codegen_.bytesLowerer().bytesLength(self.value);
    if (call->mayBeOutOfRange) {
      // 0 <= offset and offset + size <= length, as unsigned numbers.
      llvm::Value *ok = codegen_.builder_.CreateAnd(
          codegen_.builder_.CreateICmpULT(offset, length),
          codegen_.builder_.CreateICmpULE(codegen_.builder_.getInt64(size), codegen_.builder_.CreateSub(length, offset)));
      llvm::BasicBlock *badBlock = codegen_.functionFrame().newBlock("bytes.out.of.range");
      llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("bytes.in.range");
      codegen_.builder_.CreateCondBr(ok, okBlock, badBlock,
                            llvm::MDBuilder(codegen_.ctx_).createLikelyBranchWeights());
      codegen_.functionFrame().switchTo(badBlock);
      if (dataView) {
        codegen_.builder_.CreateCall(fn("tz_dataview_range_error", voidTy, {}));
      } else {
        codegen_.builder_.CreateCall(fn("tz_buffer_bounds_error", voidTy, {i64, i64, i64}),
                            {offset, codegen_.builder_.getInt64(size), length});
      }
      CodeGen::Operand released = self;
      codegen_.refCounts().dropOperand(released);
      codegen_.throwLowerer().emitRuntimeThrow();
      codegen_.functionFrame().switchTo(okBlock);
    }
    llvm::Value *address =
        codegen_.builder_.CreateInBoundsGEP(codegen_.builder_.getInt8Ty(), codegen_.bytesLowerer().bytesData(self.value), offset);
    llvm::Type *bits = codegen_.builder_.getIntNTy(size * 8);
    auto swapped = [&](llvm::Value *raw) -> llvm::Value * {
      if (size == 1) {
        return raw;
      }
      llvm::Value *swap = codegen_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, raw);
      // The bytes lie little-endian in memory.
      if (auto *constant = dyn_cast<llvm::ConstantInt>(little)) {
        return constant->isOne() ? raw : swap;
      }

      return codegen_.builder_.CreateSelect(little, raw, swap);
    };
    llvm::Value *result;
    if (!write) {
      llvm::LoadInst *raw = codegen_.builder_.CreateAlignedLoad(bits, address, llvm::Align(1));
      raw->setMetadata(llvm::LLVMContext::MD_tbaa, codegen_.bytesLowerer().bytesTbaa(/*header=*/false));
      llvm::Value *v = swapped(raw);
      switch (kind) {
      case Bytes::Float32:
        result = codegen_.builder_.CreateFPExt(codegen_.builder_.CreateBitCast(v, codegen_.builder_.getFloatTy()), f64);
        break;
      case Bytes::Float64:
        result = codegen_.builder_.CreateBitCast(v, f64);
        break;
      case Bytes::Float16:
        result =
            codegen_.builder_.CreateCall(fn("tz_f16_to_f64", f64, {i32}), {codegen_.builder_.CreateZExt(v, i32)});
        break;
      case Bytes::Uint32:
        result = codegen_.builder_.CreateZExt(v, i64);
        break;
      case Bytes::Int32:
        result = v;
        break;
      default:
        result = isSignedKind(kind) ? codegen_.builder_.CreateSExt(v, i32) : codegen_.builder_.CreateZExt(v, i32);
        break;
      }
    } else {
      llvm::Value *v;
      switch (kind) {
      case Bytes::Float32:
        v = codegen_.builder_.CreateBitCast(codegen_.builder_.CreateFPTrunc(value, codegen_.builder_.getFloatTy()), bits);
        break;
      case Bytes::Float64:
        v = codegen_.builder_.CreateBitCast(value, bits);
        break;
      case Bytes::Float16:
        v = codegen_.builder_.CreateTrunc(codegen_.builder_.CreateCall(fn("tz_f16_from_f64", i32, {f64}), {value}),
                                 bits);
        break;
      default:
        v = value->getType() == bits ? value : codegen_.builder_.CreateTrunc(value, bits);
        break;
      }
      llvm::StoreInst *store = codegen_.builder_.CreateAlignedStore(swapped(v), address, llvm::Align(1));
      store->setMetadata(llvm::LLVMContext::MD_tbaa, codegen_.bytesLowerer().bytesTbaa(/*header=*/false));
      // A Buffer's write gives the offset after what it wrote.
      result = dataView
                   ? nullptr
                   : codegen_.builder_.CreateTrunc(codegen_.builder_.CreateAdd(offset, codegen_.builder_.getInt64(size)), i32);
    }
    codegen_.refCounts().dropOperand(self);
    return result;
  }
  case Builtin::BufferReadVar:
  case Builtin::BufferWriteVar: {
    bool write = call->builtin == Builtin::BufferWriteVar;
    CodeGen::Operand self = receiver(2);
    std::vector<llvm::Value *> values;
    for (size_t i = 2; i < call->args.size(); ++i) {
      values.push_back(codegen_.exprLowerer().emitExpr(call->args[i]));
    }
    codegen_.throwLowerer().takeOperand(self);
    uint32_t flags = uint32_t(cast<BoolLitExpr>(call->args[0])->value) |
                     uint32_t(cast<BoolLitExpr>(call->args[1])->value) << 1;
    llvm::Value *result;
    if (write) {
      result = codegen_.builder_.CreateCall(
          fn("tz_buffer_write_var", i64, {ptr, i64, i64, i64, i32}),
          {self.value, values[0], values[1], values[2], codegen_.builder_.getInt32(flags)});
      result = codegen_.builder_.CreateTrunc(result, i32);
    } else {
      result = codegen_.builder_.CreateCall(fn("tz_buffer_read_var", i64, {ptr, i64, i64, i32}),
                                   {self.value, values[0], values[1], codegen_.builder_.getInt32(flags)});
    }
    codegen_.refCounts().dropOperand(self);
    codegen_.throwLowerer().emitThrowCheck();
    return result;
  }
  case Builtin::BufferFromString: {
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], AssignLowerer::cannotAssign(call->args[1]));
    codegen_.throwLowerer().holdOperand(text, throws || codegen_.mayThrow().exprThrows(call->args[1]));
    llvm::Value *enc = encoding(call->args[1]);
    codegen_.throwLowerer().takeOperand(text);
    llvm::Value *made =
        codegen_.builder_.CreateCall(fn("tz_buffer_from_string", ptr, {ptr, i64, i32}),
                            {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value), enc}, "buffer");
    codegen_.refCounts().dropOperand(text);
    return made;
  }
  case Builtin::BufferAllocFill: {
    llvm::Value *size = codegen_.exprLowerer().emitExpr(call->args[0]);
    Expr *fillExpr = call->args[1];
    CodeGen::Operand fill = codegen_.refCounts().emitOperand(fillExpr, AssignLowerer::cannotAssign(call->args[2]));
    codegen_.throwLowerer().holdOperand(fill, throws || codegen_.mayThrow().exprThrows(call->args[2]));
    llvm::Value *enc = encoding(call->args[2]);
    codegen_.throwLowerer().takeOperand(fill);
    llvm::Value *made = codegen_.builder_.CreateCall(fn("tz_buffer_alloc", ptr, {i64, i32}),
                                            {size, codegen_.builder_.getInt32(1)}, "buffer");
    llvm::Value *zero = codegen_.builder_.getInt64(0), *none = codegen_.builder_.getInt32(0);
    // A buffer that failed is NULL: nothing to fill, and the error goes up.
    llvm::BasicBlock *fillBlock = codegen_.functionFrame().newBlock("alloc.fill");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("alloc.end");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(made), fillBlock, endBlock);
    codegen_.functionFrame().switchTo(fillBlock);
    if (fillExpr->type->isString()) {
      codegen_.builder_.CreateCall(
          fn("tz_buffer_fill_string", voidTy, {ptr, ptr, i64, i64, i64, i32, i32}),
          {made, codegen_.stringLowerer().stringObj(fill.value), codegen_.stringLowerer().stringMeta(fill.value), zero, zero, none, enc});
    } else if (fillExpr->type->isInt()) {
      codegen_.builder_.CreateCall(fn("tz_buffer_fill_byte", voidTy, {ptr, i32, i64, i64, i32}),
                          {made, fill.value, zero, zero, none});
    } else {
      codegen_.builder_.CreateCall(fn("tz_buffer_fill_bytes", voidTy, {ptr, ptr, i64, i64, i32}),
                          {made, fill.value, zero, zero, none});
    }
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    codegen_.refCounts().dropOperand(fill);
    CodeGen::Held held = codegen_.throwLowerer().hold(made, type, true);
    codegen_.throwLowerer().emitThrowCheck();
    return codegen_.throwLowerer().take(held);
  }
  case Builtin::BufferConcat: {
    CodeGen::Operand list = codegen_.refCounts().emitOperand(call->args[0], call->args.size() < 2 || AssignLowerer::cannotAssign(call->args[1]));
    codegen_.throwLowerer().holdOperand(list, call->args.size() > 1 && codegen_.mayThrow().exprThrows(call->args[1]));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 1, values);
    codegen_.throwLowerer().takeOperand(list);
    llvm::Value *made = codegen_.builder_.CreateCall(fn("tz_buffer_concat", ptr, {ptr, i64, i32}),
                                            {list.value, values[0], has}, "buffer");
    codegen_.refCounts().dropOperand(list);
    codegen_.throwLowerer().emitThrowCheck();
    return made;
  }
  case Builtin::BufferByteLength: {
    Expr *source = call->args[0];
    if (source->type->isString()) {
      CodeGen::Operand text = codegen_.refCounts().emitOperand(source, AssignLowerer::cannotAssign(call->args[1]));
      codegen_.throwLowerer().holdOperand(text, throws || codegen_.mayThrow().exprThrows(call->args[1]));
      llvm::Value *enc = encoding(call->args[1]);
      codegen_.throwLowerer().takeOperand(text);
      llvm::Value *length =
          codegen_.builder_.CreateCall(fn("tz_buffer_byte_length", i32, {ptr, i64, i32}),
                              {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value), enc});
      codegen_.refCounts().dropOperand(text);
      return length;
    }
    CodeGen::Operand bytes = codegen_.refCounts().emitOperand(source, /*canBorrow=*/true);
    llvm::Value *length = codegen_.bytesLowerer().bytesLength(bytes.value);
    codegen_.refCounts().dropOperand(bytes);
    unsigned size = source->type->isTypedArray() ? source->type->elementSize() : 1;
    return codegen_.builder_.CreateTrunc(codegen_.builder_.CreateMul(length, codegen_.builder_.getInt64(size)), i32);
  }
  case Builtin::BufferCompare:
  case Builtin::BufferEquals: {
    Expr *first = call->builtin == Builtin::BufferCompare ? call->args[0] : member->object;
    Expr *second = call->builtin == Builtin::BufferCompare ? call->args[1] : call->args[0];
    CodeGen::Operand a = codegen_.refCounts().emitOperand(first, AssignLowerer::cannotAssign(second));
    codegen_.throwLowerer().holdOperand(a, codegen_.mayThrow().exprThrows(second));
    CodeGen::Operand b = codegen_.refCounts().emitOperand(second, /*canBorrow=*/true);
    codegen_.throwLowerer().takeOperand(a);
    llvm::Value *result =
        call->builtin == Builtin::BufferCompare
            ? codegen_.builder_.CreateCall(fn("tz_bytes_compare", i32, {ptr, ptr}), {a.value, b.value})
            : codegen_.builder_.CreateICmpNE(
                  codegen_.builder_.CreateCall(fn("tz_bytes_equal", i32, {ptr, ptr}), {a.value, b.value}),
                  codegen_.builder_.getInt32(0));
    codegen_.refCounts().dropOperand(b);
    codegen_.refCounts().dropOperand(a);
    return result;
  }
  case Builtin::BufferCompareTo: {
    CodeGen::Operand self = receiver(0);
    CodeGen::Operand target = codegen_.refCounts().emitOperand(
        call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), AssignLowerer::cannotAssign));
    codegen_.throwLowerer().holdOperand(target, llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                     [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); }));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 4, values);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.throwLowerer().takeOperand(target);
    llvm::Value *result = codegen_.builder_.CreateCall(
        fn("tz_buffer_compare_range", i32, {ptr, ptr, i64, i64, i64, i64, i32}),
        {self.value, target.value, values[0], values[1], values[2], values[3], has});
    codegen_.refCounts().dropOperand(target);
    codegen_.refCounts().dropOperand(self);
    codegen_.throwLowerer().emitThrowCheck();
    return result;
  }
  case Builtin::BufferIsBuffer: {
    const Type *t = call->args[0]->type;
    if (!t->nonOptional()->isTypedArray() || t->nonOptional()->bytes() != Bytes::Uint8 ||
        t->isOptional()) {
      codegen_.stmtLowerer().emitDiscarded(call->args[0]);
      return codegen_.builder_.getInt1(t->isBuffer());
    }
    // A Uint8Array may be a Buffer.
    CodeGen::Operand bytes = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *is = codegen_.builder_.CreateCall(fn("tz_bytes_is_buffer", i32, {ptr}), {bytes.value});
    codegen_.refCounts().dropOperand(bytes);
    return codegen_.builder_.CreateICmpNE(is, codegen_.builder_.getInt32(0));
  }
  case Builtin::BufferIsEncoding: {
    CodeGen::Operand name = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *found = codegen_.builder_.CreateCall(fn("tz_buffer_encoding", i32, {ptr, i64}),
                                             {codegen_.stringLowerer().stringObj(name.value), codegen_.stringLowerer().stringMeta(name.value)});
    codegen_.refCounts().dropOperand(name);
    return codegen_.builder_.CreateICmpSGE(found, codegen_.builder_.getInt32(0));
  }
  case Builtin::BufferToString: {
    CodeGen::Operand self = receiver(0);
    llvm::Value *enc = encoding(call->args[0]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 2, values);
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *text =
        codegen_.stringLowerer().callReturningString(fn("tz_buffer_to_string", voidTy, {ptr, ptr, i32, i64, i64, i32}),
                            {self.value, enc, values[0], values[1], has});
    codegen_.refCounts().dropOperand(self);
    return text;
  }
  case Builtin::BufferWrite: {
    CodeGen::Operand self = receiver(0);
    CodeGen::Operand text = codegen_.refCounts().emitOperand(
        call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), AssignLowerer::cannotAssign));
    codegen_.throwLowerer().holdOperand(text, throws || llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                             [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); }));
    llvm::Value *enc = encoding(call->args[1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(2, 2, values);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.throwLowerer().takeOperand(text);
    llvm::Value *written =
        codegen_.builder_.CreateCall(fn("tz_buffer_write_string", i32, {ptr, ptr, i64, i64, i64, i32, i32}),
                            {self.value, codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value), values[0],
                             values[1], has, enc});
    codegen_.refCounts().dropOperand(text);
    codegen_.refCounts().dropOperand(self);
    codegen_.throwLowerer().emitThrowCheck();
    return written;
  }
  case Builtin::BufferIndexOf:
  case Builtin::BufferFill: {
    bool fill = call->builtin == Builtin::BufferFill;
    size_t valueAt = fill ? 0 : 1;
    Expr *valueExpr = call->args[valueAt];
    CodeGen::Operand self = receiver(valueAt);
    CodeGen::Operand value = codegen_.refCounts().emitOperand(
        valueExpr,
        llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(valueAt + 1), AssignLowerer::cannotAssign));
    codegen_.throwLowerer().holdOperand(value,
                throws || llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(valueAt + 1),
                                       [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); }));
    llvm::Value *enc = encoding(call->args[valueAt + 1]);
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(valueAt + 2, fill ? 2 : 1, values);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.throwLowerer().takeOperand(value);
    llvm::Value *result = nullptr;
    if (fill) {
      if (valueExpr->type->isString()) {
        codegen_.builder_.CreateCall(
            fn("tz_buffer_fill_string", voidTy, {ptr, ptr, i64, i64, i64, i32, i32}),
            {self.value, codegen_.stringLowerer().stringObj(value.value), codegen_.stringLowerer().stringMeta(value.value), values[0], values[1], has,
             enc});
      } else if (valueExpr->type->isInt()) {
        codegen_.builder_.CreateCall(fn("tz_buffer_fill_byte", voidTy, {ptr, i32, i64, i64, i32}),
                            {self.value, value.value, values[0], values[1], has});
      } else {
        codegen_.builder_.CreateCall(fn("tz_buffer_fill_bytes", voidTy, {ptr, ptr, i64, i64, i32}),
                            {self.value, value.value, values[0], values[1], has});
      }
      codegen_.refCounts().dropOperand(value);
      CodeGen::Held held = codegen_.throwLowerer().hold(returnSelf(self), self.type, true);
      codegen_.throwLowerer().emitThrowCheck();
      return codegen_.throwLowerer().take(held);
    }
    llvm::Value *mode = codegen_.builder_.getInt32(uint32_t(constantOf(call->args[0])));
    if (valueExpr->type->isString()) {
      result = codegen_.builder_.CreateCall(
          fn("tz_buffer_index_of_string", i32, {ptr, ptr, i64, i64, i32, i32, i32}),
          {self.value, codegen_.stringLowerer().stringObj(value.value), codegen_.stringLowerer().stringMeta(value.value), values[0], has, enc, mode});
    } else if (valueExpr->type->isInt()) {
      result = codegen_.builder_.CreateCall(fn("tz_buffer_index_of_byte", i32, {ptr, i32, i64, i32, i32}),
                                   {self.value, value.value, values[0], has, mode});
    } else {
      result = codegen_.builder_.CreateCall(fn("tz_buffer_index_of_bytes", i32, {ptr, ptr, i64, i32, i32}),
                                   {self.value, value.value, values[0], has, mode});
    }
    codegen_.refCounts().dropOperand(value);
    codegen_.refCounts().dropOperand(self);
    return constantOf(call->args[0]) == 2 ? codegen_.builder_.CreateICmpNE(result, codegen_.builder_.getInt32(0))
                                          : result;
  }
  case Builtin::BufferCopy: {
    CodeGen::Operand self = receiver(0);
    CodeGen::Operand target = codegen_.refCounts().emitOperand(
        call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), AssignLowerer::cannotAssign));
    codegen_.throwLowerer().holdOperand(target, llvm::any_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                     [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); }));
    std::vector<llvm::Value *> values;
    llvm::Value *has = positions(1, 3, values);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.throwLowerer().takeOperand(target);
    llvm::Value *copied =
        codegen_.builder_.CreateCall(fn("tz_buffer_copy", i32, {ptr, ptr, i64, i64, i64, i32}),
                            {self.value, target.value, values[0], values[1], values[2], has});
    codegen_.refCounts().dropOperand(target);
    codegen_.refCounts().dropOperand(self);
    codegen_.throwLowerer().emitThrowCheck();
    return copied;
  }
  case Builtin::BufferSwap: {
    CodeGen::Operand self = receiver(0);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.builder_.CreateCall(fn("tz_buffer_swap", voidTy, {ptr, i32}),
                        {self.value, codegen_.builder_.getInt32(uint32_t(constantOf(call->args[0])))});
    CodeGen::Held held = codegen_.throwLowerer().hold(returnSelf(self), self.type, true);
    codegen_.throwLowerer().emitThrowCheck();
    return codegen_.throwLowerer().take(held);
  }
  case Builtin::BufferToJSON: {
    CodeGen::Operand self = receiver(0);
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *object = codegen_.objectLayouts().allocObject(type, {0, 1});
    codegen_.builder_.CreateStore(codegen_.stringLowerer().stringLiteral(u"Buffer"), codegen_.objectLayouts().fieldAddress(object, type, "type"));
    llvm::Value *data = codegen_.builder_.CreateCall(fn("tz_bytes_to_array", ptr, {ptr, ptr}),
                                            {self.value, codegen_.typeDescriptors().typeDescriptor(Type::intTy())});
    codegen_.builder_.CreateStore(data, codegen_.objectLayouts().fieldAddress(object, type, "data"));
    codegen_.refCounts().dropOperand(self);
    return object;
  }
  case Builtin::TextEncoderNew:
    return codegen_.builder_.CreateCall(fn("tz_text_encoder_new", ptr, {}), {}, "encoder");
  case Builtin::TextEncode: {
    CodeGen::Operand self = receiver(0);
    if (call->args.empty()) {
      llvm::Value *empty = codegen_.stringLowerer().stringLiteral(u"");
      codegen_.throwLowerer().takeOperand(self);
      codegen_.refCounts().dropOperand(self);
      return codegen_.builder_.CreateCall(fn("tz_text_encode", ptr, {ptr, i64}),
                                 {codegen_.stringLowerer().stringObj(empty), codegen_.stringLowerer().stringMeta(empty)}, "bytes");
    }
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *made =
        codegen_.builder_.CreateCall(fn("tz_text_encode", ptr, {ptr, i64}),
                            {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value)}, "bytes");
    codegen_.refCounts().dropOperand(text);
    codegen_.refCounts().dropOperand(self);
    return made;
  }
  case Builtin::TextEncodeInto: {
    CodeGen::Operand self = receiver(0);
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], AssignLowerer::cannotAssign(call->args[1]));
    codegen_.throwLowerer().holdOperand(text, throws || codegen_.mayThrow().exprThrows(call->args[1]));
    CodeGen::Operand target = codegen_.refCounts().emitOperand(call->args[1], /*canBorrow=*/true);
    codegen_.throwLowerer().takeOperand(self);
    codegen_.throwLowerer().takeOperand(text);
    auto *pairTy = llvm::ArrayType::get(i32, 2);
    llvm::AllocaInst *out = codegen_.functionFrame().createStackSlot(pairTy, "encoded");
    codegen_.builder_.CreateCall(fn("tz_text_encode_into", voidTy, {ptr, i64, ptr, ptr}),
                        {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value), target.value, out});
    codegen_.refCounts().dropOperand(target);
    codegen_.refCounts().dropOperand(text);
    codegen_.refCounts().dropOperand(self);
    llvm::Value *object = codegen_.objectLayouts().allocObject(type, {0, 1});
    codegen_.builder_.CreateStore(
        codegen_.builder_.CreateLoad(i32, codegen_.builder_.CreateConstInBoundsGEP2_32(pairTy, out, 0, 0)),
        codegen_.objectLayouts().fieldAddress(object, type, "read"));
    codegen_.builder_.CreateStore(
        codegen_.builder_.CreateLoad(i32, codegen_.builder_.CreateConstInBoundsGEP2_32(pairTy, out, 0, 1)),
        codegen_.objectLayouts().fieldAddress(object, type, "written"));
    return object;
  }
  case Builtin::TextDecoderNew: {
    // The options first, as JavaScript reads them after the label.
    bool resolved = constantOf(call->args[0]) != 0;
    CodeGen::Operand label{nullptr, false, nullptr};
    if (!resolved) {
      label = codegen_.refCounts().emitOperand(call->args[1], call->args.size() < 3 || AssignLowerer::cannotAssign(call->args[2]));
      codegen_.throwLowerer().holdOperand(label, call->args.size() > 2 && codegen_.mayThrow().exprThrows(call->args[2]));
    }
    llvm::Value *fatal = codegen_.builder_.getInt32(0), *ignore = codegen_.builder_.getInt32(0);
    if (call->args.size() > 2) {
      CodeGen::Operand options = codegen_.refCounts().emitOperand(call->args[2], /*canBorrow=*/true);
      const Type *optionsType = options.type;
      auto flag = [&](llvm::StringRef name) -> llvm::Value * {
        const Type *t = optionsType->field(name)->type;
        llvm::Value *loaded =
            codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(t), codegen_.objectLayouts().fieldAddress(options.value, optionsType, name));
        llvm::Value *value = codegen_.builder_.CreateAnd(codegen_.optionalLowerer().optionalHas(loaded, t), codegen_.optionalLowerer().optionalValue(loaded, t));

        return codegen_.builder_.CreateZExt(value, i32);
      };
      fatal = flag("fatal");
      ignore = flag("ignoreBOM");
      codegen_.refCounts().dropOperand(options);
    }
    if (resolved) {
      std::string name;
      for (char16_t c : cast<StringLitExpr>(call->args[1])->value) {
        name += char(c);
      }
      bool multi = tz_encoding_multi_byte(name.c_str());
      int which = name == "utf-8"      ? kTextUtf8
                  : name == "utf-16le" ? kTextUtf16le
                  : name == "utf-16be" ? kTextUtf16be
                  : multi              ? kTextCjk
                                       : kTextSingleByte;
      llvm::Value *table = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
      if (which == kTextSingleByte) {
        table = codegen_.bytesLowerer().encodingTable(name);
      } else if (which == kTextCjk) {
        table = cjkEncodingOf(*codegen_.module_, name);
      }
      return codegen_.builder_.CreateCall(fn("tz_text_decoder_make", ptr, {i32, ptr, i32, i32}),
                                 {codegen_.builder_.getInt32(uint32_t(which)), table, fatal, ignore},
                                 "decoder");
    }
    // A label known only now: the program holds every table.
    std::vector<llvm::Constant *> tables, cjk;
    for (const EncodingTable &t : kEncodingTables) {
      tables.push_back(codegen_.bytesLowerer().encodingTable(t.name));
    }
    for (const char *name : kCjkEncodings) {
      cjk.push_back(cjkEncodingOf(*codegen_.module_, name));
    }
    auto list = [&](std::vector<llvm::Constant *> &items, const char *name) {
      auto *type = llvm::ArrayType::get(codegen_.builder_.getPtrTy(), items.size());

      return new llvm::GlobalVariable(*codegen_.module_, type, /*isConstant=*/true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      llvm::ConstantArray::get(type, items), name);
    };
    llvm::Value *all = list(tables, "tz.encodings"), *allCjk = list(cjk, "tz.encodings.cjk");
    codegen_.throwLowerer().takeOperand(label);
    llvm::Value *made = codegen_.builder_.CreateCall(
        fn("tz_text_decoder_new", ptr, {ptr, i64, ptr, i32, ptr, i32, i32, i32}),
        {codegen_.stringLowerer().stringObj(label.value), codegen_.stringLowerer().stringMeta(label.value), all,
         codegen_.builder_.getInt32(uint32_t(tables.size())), allCjk,
         codegen_.builder_.getInt32(uint32_t(cjk.size())), fatal, ignore},
        "decoder");
    codegen_.refCounts().dropOperand(label);
    codegen_.throwLowerer().emitThrowCheck(); // RangeError: the encoding is not supported
    return made;
  }
  case Builtin::TextDecode: {
    CodeGen::Operand self = receiver(0);
    llvm::Value *data = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
    llvm::Value *length = codegen_.builder_.getInt64(0);
    CodeGen::Operand input{nullptr, false, nullptr};
    if (!call->args.empty() && !call->args[0]->type->isUndefined()) {
      input = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      data = codegen_.bytesLowerer().bytesData(input.value);
      length = codegen_.bytesLowerer().bytesLength(input.value);
      if (input.type->isTypedArray()) {
        length = codegen_.builder_.CreateMul(length, codegen_.builder_.getInt64(input.type->elementSize()));
      }
    }
    // `{ stream: true }`: the text goes on in the next call.
    llvm::Value *stream = codegen_.builder_.getInt32(0);
    if (call->args.size() > 1) {
      CodeGen::Operand options = codegen_.refCounts().emitOperand(call->args[1], /*canBorrow=*/true);
      const Type *t = options.type->field("stream")->type;
      llvm::Value *loaded =
          codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(t), codegen_.objectLayouts().fieldAddress(options.value, options.type, "stream"));
      llvm::Value *given = codegen_.builder_.CreateAnd(codegen_.optionalLowerer().optionalHas(loaded, t), codegen_.optionalLowerer().optionalValue(loaded, t));
      stream = codegen_.builder_.CreateZExt(given, i32);
      codegen_.refCounts().dropOperand(options);
    }
    codegen_.throwLowerer().takeOperand(self);
    llvm::Value *text = codegen_.stringLowerer().callReturningString(fn("tz_text_decode", voidTy, {ptr, ptr, ptr, i64, i32}),
                                            {self.value, data, length, stream});
    if (input.value) {
      codegen_.refCounts().dropOperand(input);
    }
    codegen_.refCounts().dropOperand(self);
    codegen_.throwLowerer().emitThrowCheck(); // TypeError of a fatal decoder
    return text;
  }
  case Builtin::TextDecoderInfo: {
    CodeGen::Operand self = receiver(0);
    codegen_.throwLowerer().takeOperand(self);
    int64_t which = constantOf(call->args[0]);
    llvm::Value *result;
    if (which == 0) {
      result =
          codegen_.stringLowerer().callReturningString(fn("tz_text_decoder_encoding", voidTy, {ptr, ptr}), {self.value});
    } else {
      result = codegen_.builder_.CreateICmpNE(
          codegen_.builder_.CreateCall(fn("tz_text_decoder_flag", i32, {ptr, i32}),
                              {self.value, codegen_.builder_.getInt32(uint32_t(which - 1))}),
          codegen_.builder_.getInt32(0));
    }
    codegen_.refCounts().dropOperand(self);
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
      for (char16_t unit : units) {
        bytes.push_back(uint8_t(unit));
      }
      llvm::Constant *init = llvm::ConstantDataArray::get(codegen_.ctx_, bytes);
      auto *data =
          new llvm::GlobalVariable(*codegen_.module_, init->getType(), /*isConstant=*/false,
                                   llvm::GlobalValue::PrivateLinkage, init, "tz.embedded.data");
      data->setAlignment(llvm::Align(16));
      auto *layoutsTy = llvm::ArrayType::get(codegen_.layoutDescTy_, 16);
      auto *layouts =
          cast<llvm::GlobalVariable>(codegen_.module_->getOrInsertGlobal("tz_bytes_layouts", layoutsTy));
      auto layout = [&](Bytes kind) {
        return llvm::ConstantExpr::getInBoundsGetElementPtr(
            layoutsTy, layouts,
            llvm::ArrayRef<llvm::Constant *>{codegen_.builder_.getInt32(0),
                                             codegen_.builder_.getInt32(uint32_t(kind))});
      };
      // The count of a constant never reaches zero (as for regexes).
      llvm::Constant *count = codegen_.builder_.getInt64(uint64_t(1) << 62);
      llvm::Constant *length = codegen_.builder_.getInt32(uint32_t(bytes.size()));
      auto *abufTy = llvm::StructType::get(codegen_.ctx_, {i64, ptr, ptr, i32, i32});
      auto *abuf = new llvm::GlobalVariable(
          *codegen_.module_, abufTy, /*isConstant=*/false, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantStruct::get(abufTy, {count, layout(Bytes::ArrayBuffer), data, length,
                                             codegen_.builder_.getInt32(1)}), // TZ_ABUF_STATIC
          "tz.embedded.buffer");
      if (!codegen_.bytesTy_) {
        codegen_.bytesTy_ = bytesStruct(codegen_.ctx_);
      }
      global = new llvm::GlobalVariable(
          *codegen_.module_, codegen_.bytesTy_, /*isConstant=*/false, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantStruct::get(
              codegen_.bytesTy_, {count, layout(Bytes::Uint8), data, length, codegen_.builder_.getInt32(0), abuf}),
          "tz.embedded");
    }
    return global;
  }
  default:
    llvm_unreachable("not a built-in of binary data");
  }
}

} // namespace tz
