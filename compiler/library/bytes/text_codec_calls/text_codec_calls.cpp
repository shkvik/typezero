#include "compiler/library/bytes/text_codec_calls/text_codec_calls.h"
#include "compiler/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
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

struct EncodingTable {
  const char *name;
  uint16_t high[128];
};

#define TZ_ENCODING_TABLE(name, ...) {name, {__VA_ARGS__}},

const EncodingTable kEncodingTables[] = {TZ_ENCODING_TABLES(TZ_ENCODING_TABLE)};

#undef TZ_ENCODING_TABLE

enum { kTextUtf8, kTextUtf16le, kTextUtf16be, kTextSingleByte, kTextCjk };

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

}

llvm::Value *TextCodecCalls::emit() {
  switch (call->builtin()) {
    case Builtin::TextEncoderNew:
      return this->lowering.builder_.CreateCall(this->fn("tz_text_encoder_new", ptr, {}), {},
                                                "encoder");
    case Builtin::TextEncode: {
      Lowering::Operand self = this->receiver(0);
      if (call->args.empty()) {
        llvm::Value *empty = this->lowering.stringLowerer().stringLiteral(u"");
        this->lowering.refCounts().dropOperand(self);
        return this->lowering.builder_.CreateCall(
            this->fn("tz_text_encode", ptr, {ptr, i64}),
            {this->lowering.stringLowerer().stringObj(empty),
             this->lowering.stringLowerer().stringMeta(empty)},
            "bytes");
      }
      Lowering::Operand text = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *made = this->lowering.builder_.CreateCall(
          this->fn("tz_text_encode", ptr, {ptr, i64}),
          {this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value)},
          "bytes");
      this->lowering.refCounts().dropOperand(text);
      this->lowering.refCounts().dropOperand(self);
      return made;
    }
    case Builtin::TextEncodeInto: {
      Lowering::Operand self = this->receiver(0);
      Lowering::Operand text = this->lowering.refCounts().emitOperand(
          call->args[0], AssignLowerer::cannotAssign(call->args[1]));
      Lowering::Operand target = this->lowering.refCounts().emitOperand(call->args[1], true);
      auto *pairTy = llvm::ArrayType::get(i32, 2);
      llvm::AllocaInst *out = this->lowering.functionFrame().createStackSlot(pairTy, "encoded");
      this->lowering.builder_.CreateCall(
          this->fn("tz_text_encode_into", voidTy, {ptr, i64, ptr, ptr}),
          {this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value), target.value, out});
      this->lowering.refCounts().dropOperand(target);
      this->lowering.refCounts().dropOperand(text);
      this->lowering.refCounts().dropOperand(self);
      llvm::Value *object = this->lowering.objectLayouts().allocObject(type, {0, 1});
      for (unsigned k = 0; k < 2; ++k) {
        this->lowering.builder_.CreateStore(
            this->lowering.builder_.CreateSExt(
                this->lowering.builder_.CreateLoad(
                    i32, this->lowering.builder_.CreateConstInBoundsGEP2_32(pairTy, out, 0, k)),
                i64),
            this->lowering.objectLayouts().fieldAddress(object, type, k == 0 ? "read" : "written"));
      }
      return object;
    }
    case Builtin::TextDecoderNew:
    case Builtin::TextDecoderParse: {
      bool parsing = call->builtin() == Builtin::TextDecoderParse;
      bool resolved = constantOf(call->args[0]) != 0;
      Lowering::Operand label{nullptr, false, nullptr};
      if (!resolved) {
        label = this->lowering.refCounts().emitOperand(
            call->args[1], call->args.size() < 3 || AssignLowerer::cannotAssign(call->args[2]));
      }
      llvm::Value *fatal = this->lowering.builder_.getInt32(0),
                  *ignore = this->lowering.builder_.getInt32(0);
      if (call->args.size() > 2) {
        Lowering::Operand options = this->lowering.refCounts().emitOperand(call->args[2], true);
        const Type *optionsType = options.type;
        auto flag = [&](llvm::StringRef name) -> llvm::Value * {
          const Type *t = optionsType->field(name)->type;
          llvm::Value *loaded = this->lowering.builder_.CreateLoad(
              this->lowering.moduleBuilder().lower(t),
              this->lowering.objectLayouts().fieldAddress(options.value, optionsType, name));
          llvm::Value *value = this->lowering.builder_.CreateAnd(
              this->lowering.optionalLowerer().optionalHas(loaded, t),
              this->lowering.optionalLowerer().optionalValue(loaded, t));

          return this->lowering.builder_.CreateZExt(value, i32);
        };
        fatal = flag("fatal");
        ignore = flag("ignoreBOM");
        this->lowering.refCounts().dropOperand(options);
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
        llvm::Value *table = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
        if (which == kTextSingleByte) {
          table = this->lowering.bytesLowerer().encodingTable(name);
        } else if (which == kTextCjk) {
          table = cjkEncodingOf(*this->lowering.module_, name);
        }
        llvm::Value *made = this->lowering.builder_.CreateCall(
            this->fn("tz_text_decoder_make", ptr, {i32, ptr, i32, i32}),
            {this->lowering.builder_.getInt32(uint32_t(which)), table, fatal, ignore}, "decoder");
        return parsing ? this->lowering.resultLowerer().catchFailure(made, call->type, call->loc())
                       : made;
      }
      std::vector<llvm::Constant *> tables, cjk;
      for (const EncodingTable &t : kEncodingTables) {
        tables.push_back(this->lowering.bytesLowerer().encodingTable(t.name));
      }
      for (const char *name : kCjkEncodings) {
        cjk.push_back(cjkEncodingOf(*this->lowering.module_, name));
      }
      auto list = [&](std::vector<llvm::Constant *> &items, const char *name) {
        auto *type = llvm::ArrayType::get(this->lowering.builder_.getPtrTy(), items.size());

        return new llvm::GlobalVariable(*this->lowering.module_, type, true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        llvm::ConstantArray::get(type, items), name);
      };
      llvm::Value *all = list(tables, "tz.encodings"), *allCjk = list(cjk, "tz.encodings.cjk");
      llvm::Value *maker =
          parsing ? static_cast<llvm::Value *>(
                        this->lowering.resultLowerer().failureMaker(call->type->errType()))
                  : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
      llvm::Value *made = this->lowering.builder_.CreateCall(
          this->fn("tz_text_decoder_new", ptr, {ptr, i64, ptr, i32, ptr, i32, i32, i32, ptr}),
          {this->lowering.stringLowerer().stringObj(label.value),
           this->lowering.stringLowerer().stringMeta(label.value), all,
           this->lowering.builder_.getInt32(uint32_t(tables.size())), allCjk,
           this->lowering.builder_.getInt32(uint32_t(cjk.size())), fatal, ignore, maker},
          "decoder");
      this->lowering.refCounts().dropOperand(label);
      return parsing ? this->lowering.resultLowerer().catchFailure(made, call->type, call->loc())
                     : made;
    }
    case Builtin::TextDecode: {
      Lowering::Operand self = this->receiver(0);
      llvm::Value *data = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
      llvm::Value *length = this->lowering.builder_.getInt64(0);
      Lowering::Operand input{nullptr, false, nullptr};
      if (!call->args.empty() && !call->args[0]->type->isUndefined()) {
        input = this->lowering.refCounts().emitOperand(call->args[0], true);
        data = this->lowering.bytesLowerer().bytesData(input.value);
        length = this->lowering.bytesLowerer().bytesLength(input.value);
        if (input.type->isTypedArray()) {
          length = this->lowering.builder_.CreateMul(
              length, this->lowering.builder_.getInt64(input.type->elementSize()));
        }
      }
      llvm::Value *stream = this->lowering.builder_.getInt32(0);
      if (call->args.size() > 1) {
        Lowering::Operand options = this->lowering.refCounts().emitOperand(call->args[1], true);
        const Type *t = options.type->field("stream")->type;
        llvm::Value *loaded = this->lowering.builder_.CreateLoad(
            this->lowering.moduleBuilder().lower(t),
            this->lowering.objectLayouts().fieldAddress(options.value, options.type, "stream"));
        llvm::Value *given = this->lowering.builder_.CreateAnd(
            this->lowering.optionalLowerer().optionalHas(loaded, t),
            this->lowering.optionalLowerer().optionalValue(loaded, t));
        stream = this->lowering.builder_.CreateZExt(given, i32);
        this->lowering.refCounts().dropOperand(options);
      }
      bool checked = call->type->isResult();
      llvm::Value *make = llvm::ConstantPointerNull::get(ptr);
      if (checked) {
        make = this->lowering.resultLowerer().failureMaker(call->type->errType());
      }
      llvm::Value *text = this->lowering.stringLowerer().callReturningString(
          this->fn("tz_text_decode", voidTy, {ptr, ptr, ptr, i64, i32, ptr}),
          {self.value, data, length, stream, make});
      if (input.value) {
        this->lowering.refCounts().dropOperand(input);
      }
      this->lowering.refCounts().dropOperand(self);

      return checked ? this->lowering.resultLowerer().catchFailure(text, call->type, call->loc())
                     : text;
    }
    case Builtin::TextDecoderInfo: {
      Lowering::Operand self = this->receiver(0);
      int64_t which = constantOf(call->args[0]);
      llvm::Value *result;
      if (which == 0) {
        result = this->lowering.stringLowerer().callReturningString(
            this->fn("tz_text_decoder_encoding", voidTy, {ptr, ptr}), {self.value});
      } else {
        result = this->lowering.builder_.CreateICmpNE(
            this->lowering.builder_.CreateCall(
                this->fn("tz_text_decoder_flag", i32, {ptr, i32}),
                {self.value, this->lowering.builder_.getInt32(uint32_t(which - 1))}),
            this->lowering.builder_.getInt32(0));
      }
      this->lowering.refCounts().dropOperand(self);
      return result;
    }
    default:
      llvm_unreachable("not a call of TextCodecCalls");
  }
}

}
