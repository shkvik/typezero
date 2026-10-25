#include "compiler/library/bytes/text_codec_calls/text_codec_calls.h"
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

namespace {

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

} // namespace

llvm::Value *TextCodecCalls::emit() {
  switch (call->builtin) {
  case Builtin::TextEncoderNew:
    return lowering_.builder_.CreateCall(fn("tz_text_encoder_new", ptr, {}), {}, "encoder");
  case Builtin::TextEncode: {
    Lowering::Operand self = receiver(0);
    if (call->args.empty()) {
      llvm::Value *empty = lowering_.stringLowerer().stringLiteral(u"");
      lowering_.throwLowerer().takeOperand(self);
      lowering_.refCounts().dropOperand(self);
      return lowering_.builder_.CreateCall(
          fn("tz_text_encode", ptr, {ptr, i64}),
          {lowering_.stringLowerer().stringObj(empty), lowering_.stringLowerer().stringMeta(empty)},
          "bytes");
    }
    Lowering::Operand text = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *made =
        lowering_.builder_.CreateCall(fn("tz_text_encode", ptr, {ptr, i64}),
                                      {lowering_.stringLowerer().stringObj(text.value),
                                       lowering_.stringLowerer().stringMeta(text.value)},
                                      "bytes");
    lowering_.refCounts().dropOperand(text);
    lowering_.refCounts().dropOperand(self);
    return made;
  }
  case Builtin::TextEncodeInto: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand text = lowering_.refCounts().emitOperand(
        call->args[0], AssignLowerer::cannotAssign(call->args[1]));
    lowering_.throwLowerer().holdOperand(text,
                                         throws || lowering_.mayThrow().exprThrows(call->args[1]));
    Lowering::Operand target = lowering_.refCounts().emitOperand(call->args[1], /*canBorrow=*/true);
    lowering_.throwLowerer().takeOperand(self);
    lowering_.throwLowerer().takeOperand(text);
    auto *pairTy = llvm::ArrayType::get(i32, 2);
    llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(pairTy, "encoded");
    lowering_.builder_.CreateCall(fn("tz_text_encode_into", voidTy, {ptr, i64, ptr, ptr}),
                                  {lowering_.stringLowerer().stringObj(text.value),
                                   lowering_.stringLowerer().stringMeta(text.value), target.value,
                                   out});
    lowering_.refCounts().dropOperand(target);
    lowering_.refCounts().dropOperand(text);
    lowering_.refCounts().dropOperand(self);
    llvm::Value *object = lowering_.objectLayouts().allocObject(type, {0, 1});
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateLoad(
            i32, lowering_.builder_.CreateConstInBoundsGEP2_32(pairTy, out, 0, 0)),
        lowering_.objectLayouts().fieldAddress(object, type, "read"));
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateLoad(
            i32, lowering_.builder_.CreateConstInBoundsGEP2_32(pairTy, out, 0, 1)),
        lowering_.objectLayouts().fieldAddress(object, type, "written"));
    return object;
  }
  case Builtin::TextDecoderNew: {
    // The options first, as JavaScript reads them after the label.
    bool resolved = constantOf(call->args[0]) != 0;
    Lowering::Operand label{nullptr, false, nullptr};
    if (!resolved) {
      label = lowering_.refCounts().emitOperand(
          call->args[1], call->args.size() < 3 || AssignLowerer::cannotAssign(call->args[2]));
      lowering_.throwLowerer().holdOperand(
          label, call->args.size() > 2 && lowering_.mayThrow().exprThrows(call->args[2]));
    }
    llvm::Value *fatal = lowering_.builder_.getInt32(0), *ignore = lowering_.builder_.getInt32(0);
    if (call->args.size() > 2) {
      Lowering::Operand options =
          lowering_.refCounts().emitOperand(call->args[2], /*canBorrow=*/true);
      const Type *optionsType = options.type;
      auto flag = [&](llvm::StringRef name) -> llvm::Value * {
        const Type *t = optionsType->field(name)->type;
        llvm::Value *loaded = lowering_.builder_.CreateLoad(
            lowering_.moduleBuilder().lower(t),
            lowering_.objectLayouts().fieldAddress(options.value, optionsType, name));
        llvm::Value *value =
            lowering_.builder_.CreateAnd(lowering_.optionalLowerer().optionalHas(loaded, t),
                                         lowering_.optionalLowerer().optionalValue(loaded, t));

        return lowering_.builder_.CreateZExt(value, i32);
      };
      fatal = flag("fatal");
      ignore = flag("ignoreBOM");
      lowering_.refCounts().dropOperand(options);
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
      llvm::Value *table = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
      if (which == kTextSingleByte) {
        table = lowering_.bytesLowerer().encodingTable(name);
      } else if (which == kTextCjk) {
        table = cjkEncodingOf(*lowering_.module_, name);
      }
      return lowering_.builder_.CreateCall(
          fn("tz_text_decoder_make", ptr, {i32, ptr, i32, i32}),
          {lowering_.builder_.getInt32(uint32_t(which)), table, fatal, ignore}, "decoder");
    }
    // A label known only now: the program holds every table.
    std::vector<llvm::Constant *> tables, cjk;
    for (const EncodingTable &t : kEncodingTables) {
      tables.push_back(lowering_.bytesLowerer().encodingTable(t.name));
    }
    for (const char *name : kCjkEncodings) {
      cjk.push_back(cjkEncodingOf(*lowering_.module_, name));
    }
    auto list = [&](std::vector<llvm::Constant *> &items, const char *name) {
      auto *type = llvm::ArrayType::get(lowering_.builder_.getPtrTy(), items.size());

      return new llvm::GlobalVariable(*lowering_.module_, type, /*isConstant=*/true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      llvm::ConstantArray::get(type, items), name);
    };
    llvm::Value *all = list(tables, "tz.encodings"), *allCjk = list(cjk, "tz.encodings.cjk");
    lowering_.throwLowerer().takeOperand(label);
    llvm::Value *made = lowering_.builder_.CreateCall(
        fn("tz_text_decoder_new", ptr, {ptr, i64, ptr, i32, ptr, i32, i32, i32}),
        {lowering_.stringLowerer().stringObj(label.value),
         lowering_.stringLowerer().stringMeta(label.value), all,
         lowering_.builder_.getInt32(uint32_t(tables.size())), allCjk,
         lowering_.builder_.getInt32(uint32_t(cjk.size())), fatal, ignore},
        "decoder");
    lowering_.refCounts().dropOperand(label);
    lowering_.throwLowerer().emitThrowCheck(); // RangeError: the encoding is not supported
    return made;
  }
  case Builtin::TextDecode: {
    Lowering::Operand self = receiver(0);
    llvm::Value *data = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
    llvm::Value *length = lowering_.builder_.getInt64(0);
    Lowering::Operand input{nullptr, false, nullptr};
    if (!call->args.empty() && !call->args[0]->type->isUndefined()) {
      input = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      data = lowering_.bytesLowerer().bytesData(input.value);
      length = lowering_.bytesLowerer().bytesLength(input.value);
      if (input.type->isTypedArray()) {
        length = lowering_.builder_.CreateMul(
            length, lowering_.builder_.getInt64(input.type->elementSize()));
      }
    }
    // `{ stream: true }`: the text goes on in the next call.
    llvm::Value *stream = lowering_.builder_.getInt32(0);
    if (call->args.size() > 1) {
      Lowering::Operand options =
          lowering_.refCounts().emitOperand(call->args[1], /*canBorrow=*/true);
      const Type *t = options.type->field("stream")->type;
      llvm::Value *loaded = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(t),
          lowering_.objectLayouts().fieldAddress(options.value, options.type, "stream"));
      llvm::Value *given =
          lowering_.builder_.CreateAnd(lowering_.optionalLowerer().optionalHas(loaded, t),
                                       lowering_.optionalLowerer().optionalValue(loaded, t));
      stream = lowering_.builder_.CreateZExt(given, i32);
      lowering_.refCounts().dropOperand(options);
    }
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *text = lowering_.stringLowerer().callReturningString(
        fn("tz_text_decode", voidTy, {ptr, ptr, ptr, i64, i32}),
        {self.value, data, length, stream});
    if (input.value) {
      lowering_.refCounts().dropOperand(input);
    }
    lowering_.refCounts().dropOperand(self);
    lowering_.throwLowerer().emitThrowCheck(); // TypeError of a fatal decoder
    return text;
  }
  case Builtin::TextDecoderInfo: {
    Lowering::Operand self = receiver(0);
    lowering_.throwLowerer().takeOperand(self);
    int64_t which = constantOf(call->args[0]);
    llvm::Value *result;
    if (which == 0) {
      result = lowering_.stringLowerer().callReturningString(
          fn("tz_text_decoder_encoding", voidTy, {ptr, ptr}), {self.value});
    } else {
      result = lowering_.builder_.CreateICmpNE(
          lowering_.builder_.CreateCall(
              fn("tz_text_decoder_flag", i32, {ptr, i32}),
              {self.value, lowering_.builder_.getInt32(uint32_t(which - 1))}),
          lowering_.builder_.getInt32(0));
    }
    lowering_.refCounts().dropOperand(self);
    return result;
  }
  default:
    llvm_unreachable("not a call of TextCodecCalls");
  }
}

} // namespace tz
