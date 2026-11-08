#include "compiler/lower/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/library/intl/intl_lowerer/intl_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

const llvm::StringLiteral kOptionNames[TZ_IO_COUNT] = {
#define TZ_INTL_OPTION_NAME(id, name) name,
    TZ_INTL_OPTIONS(TZ_INTL_OPTION_NAME)
#undef TZ_INTL_OPTION_NAME
};

}

int IntlCallLowerer::optionIndex(llvm::StringRef name) {
  auto at = llvm::find(kOptionNames, name);

  return at == std::end(kOptionNames) ? -1 : int(at - std::begin(kOptionNames));
}

IntlCallLowerer::IntlCallLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *IntlCallLowerer::emitIntlCall(CallExpr *call) {
  llvm::Type *voidTy = this->lowering.builder.getVoidTy(),
             *i32 = this->lowering.builder.getInt32Ty(), *i64 = this->lowering.builder.getInt64Ty(),
             *f64 = this->lowering.builder.getDoubleTy(), *ptr = this->lowering.builder.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto receiver = [&](size_t from) {
    auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
    Lowering::Operand self = this->lowering.refCounts().emitOperand(
        member->object, llvm::all_of(rest, AssignLowerer::cannotAssign));

    return self;
  };
  auto stringOf = [&](llvm::StringRef name, std::vector<llvm::Value *> args,
                      std::vector<llvm::Type *> params) {
    params.insert(params.begin(), ptr);

    return this->lowering.stringLowerer().callReturningString(
        this->lowering.processLowerer().processFunction(name, voidTy, params), args);
  };
  auto at = [&](size_t i) -> Expr * { return i < call->args.size() ? call->args[i] : nullptr; };
  auto dropAll = [&](std::vector<Lowering::Operand> &held) {
    for (const Lowering::Operand &operand : held) {
      this->lowering.refCounts().dropOperand(operand);
    }
  };

  switch (call->builtin()) {
    case Builtin::IntlNew: {
      int kind = int(cast<IntLitExpr>(call->args[0])->value);
      std::vector<Lowering::Operand> held;
      llvm::Value *options = this->lowering.intlLowerer().emitIntlOptions(at(1), at(2), held);
      llvm::Value *object = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_intl_new", ptr, {i32, ptr}),
          {this->lowering.builder.getInt32(uint32_t(kind)), options}, "intl");
      dropAll(held);
      return object;
    }
    case Builtin::IntlSupportedLocales:
    case Builtin::IntlCanonicalLocales: {
      bool supported = call->builtin() == Builtin::IntlSupportedLocales;
      llvm::Value *locales = this->lowering.intlLowerer().emitLocaleArray(at(supported ? 1 : 0));
      llvm::Value *result;
      if (supported) {
        std::vector<Lowering::Operand> held;
        llvm::Value *options = this->lowering.intlLowerer().emitIntlOptions(nullptr, at(2), held);
        const std::u16string &text = cast<StringLitExpr>(call->args[0])->value;
        std::string name = ToUtf8::toUtf8(text);
        result = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_intl_supported_locales", ptr,
                                                            {ptr, ptr, ptr}),
            {locales, options, this->lowering.stringLowerer().utf8Constant(name)}, "supported");
        dropAll(held);
      } else {
        result = this->lowering.builder.CreateCall(this->lowering.processLowerer().processFunction(
                                                       "tz_intl_canonical_locales", ptr, {ptr}),
                                                   {locales}, "canonical");
      }
      this->lowering.refCounts().releaseValue(locales, Type::array(Type::string()));
      return result;
    }
    case Builtin::IntlResolvedOptions: {
      Lowering::Operand self = receiver(0);
      const Type *type = call->type;
      std::vector<unsigned> keys(type->fields().size());
      std::iota(keys.begin(), keys.end(), 0u);
      llvm::Value *object = this->lowering.objectLayouts().allocObject(type, keys);
      for (const Field &field : type->fields()) {
        llvm::Value *address =
            this->lowering.objectLayouts().fieldAddress(object, type, field.name);
        const Type *inner = field.type->nonOptional();
        llvm::Value *value;
        if (field.name == "pluralCategories") {
          value = this->lowering.builder.CreateCall(
              this->lowering.processLowerer().processFunction("tz_pr_categories", ptr, {ptr}),
              {self.value});
        } else if (field.name == "useGrouping") {
          llvm::AllocaInst *out =
              this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "grouping.out");
          llvm::Value *on = this->lowering.builder.CreateICmpNE(
              this->lowering.builder.CreateCall(this->lowering.processLowerer().processFunction(
                                                    "tz_intl_grouping", i32, {ptr, ptr}),
                                                {out, self.value}),
              this->lowering.builder.getInt32(0));
          llvm::Value *text = this->lowering.unionLayout().makeTagged(
              field.type, UnionSlot::String,
              this->lowering.builder.CreateLoad(this->lowering.strTy, out));
          llvm::Value *off = this->lowering.unionLayout().makeTagged(
              field.type, UnionSlot::Bool, this->lowering.builder.getFalse());
          value = this->lowering.builder.CreateSelect(on, text, off);
        } else if (inner->isString()) {
          int which = field.name == "locale" ? -1 : IntlCallLowerer::optionIndex(field.name);
          llvm::AllocaInst *out =
              this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "option.out");
          llvm::Value *has = this->lowering.builder.CreateICmpNE(
              this->lowering.builder.CreateCall(
                  this->lowering.processLowerer().processFunction("tz_intl_text", i32,
                                                                  {ptr, ptr, i32}),
                  {out, self.value, this->lowering.builder.getInt32(uint32_t(which))}),
              this->lowering.builder.getInt32(0));
          llvm::Value *text = this->lowering.builder.CreateLoad(this->lowering.strTy, out);
          value = field.type->isOptional()
                      ? this->lowering.optionalLowerer().makeOptional(text, has, field.type)
                      : text;
        } else {
          llvm::Value *number = this->lowering.builder.CreateCall(
              this->lowering.processLowerer().processFunction("tz_intl_number", f64, {ptr, i32}),
              {self.value, this->lowering.builder.getInt32(
                               uint32_t(IntlCallLowerer::optionIndex(field.name)))});
          llvm::Value *has = this->lowering.builder.CreateFCmpORD(number, number);
          number =
              this->lowering.builder.CreateSelect(has, number, llvm::ConstantFP::get(f64, 0.0));
          llvm::Value *v =
              inner->isBool()
                  ? this->lowering.builder.CreateFCmpUNE(number, llvm::ConstantFP::get(f64, 0.0))
                  : this->lowering.builder.CreateFPToSI(
                        number, this->lowering.moduleBuilder().lower(inner));
          value = field.type->isOptional()
                      ? this->lowering.optionalLowerer().makeOptional(v, has, field.type)
                      : v;
        }
        this->lowering.builder.CreateStore(value, address);
      }
      this->lowering.refCounts().dropOperand(self);
      return object;
    }
    case Builtin::IntlBound: {
      IntlKind kind = member->object->type->intlKind();
      llvm::Value *object = this->lowering.exprLowerer().emitExpr(member->object);
      return this->lowering.closureLowerer().makeFunctionValue(
          this->lowering.intlLowerer().intlBoundInfo(kind, call->type), object);
    }
    case Builtin::NumToLocale: {
      Lowering::IntlNumber n = this->lowering.intlLowerer().emitIntlNumber(member->object);
      std::vector<Lowering::Operand> held;
      llvm::Value *options = this->lowering.intlLowerer().emitIntlOptions(at(0), at(1), held);
      std::vector<llvm::Value *> args = this->lowering.intlLowerer().intlNumberArgs(n);
      args.push_back(options);
      llvm::Value *text = stringOf("tz_num_locale", args, {i32, f64, i64, i64, ptr});
      dropAll(held);
      this->lowering.refCounts().dropOperand(n.operand);
      return text;
    }
    case Builtin::NfFormat:
    case Builtin::NfFormatToParts:
    case Builtin::PrSelect: {
      Lowering::Operand self = receiver(0);
      Lowering::IntlNumber n = this->lowering.intlLowerer().emitIntlNumber(call->args[0]);
      std::vector<llvm::Value *> args{self.value};
      for (llvm::Value *v : this->lowering.intlLowerer().intlNumberArgs(n)) {
        args.push_back(v);
      }
      llvm::Value *result;
      if (call->builtin() == Builtin::NfFormatToParts) {
        args.push_back(this->lowering.intlLowerer().intlShape(call->type->inner()));
        result = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_nf_format_to_parts", ptr,
                                                            {ptr, i32, f64, i64, i64, ptr}),
            args, "parts");
      } else {
        result = stringOf(call->builtin() == Builtin::NfFormat ? "tz_nf_format" : "tz_pr_select",
                          args, {ptr, i32, f64, i64, i64});
      }
      this->lowering.refCounts().dropOperand(n.operand);
      this->lowering.refCounts().dropOperand(self);
      return result;
    }
    case Builtin::CollatorCompare: {
      Lowering::Operand self = receiver(0);
      Lowering::Operand a = this->lowering.refCounts().emitOperand(
          call->args[0], AssignLowerer::cannotAssign(call->args[1]));
      Lowering::Operand b = this->lowering.refCounts().emitOperand(call->args[1], true);
      llvm::Value *result = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_collator_compare", i32,
                                                          {ptr, ptr, i64, ptr, i64}),
          {self.value, this->lowering.stringLowerer().stringObj(a.value),
           this->lowering.stringLowerer().stringMeta(a.value),
           this->lowering.stringLowerer().stringObj(b.value),
           this->lowering.stringLowerer().stringMeta(b.value)},
          "order");
      this->lowering.refCounts().dropOperand(b);
      this->lowering.refCounts().dropOperand(a);
      this->lowering.refCounts().dropOperand(self);
      return this->lowering.builder.CreateSExt(result, this->lowering.builder.getInt64Ty());
    }
    case Builtin::StrLocaleCompare: {
      Lowering::Operand self = receiver(0);
      Lowering::Operand that = this->lowering.refCounts().emitOperand(
          call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(),
                                      AssignLowerer::cannotAssign));
      std::vector<Lowering::Operand> held;
      llvm::Value *options = this->lowering.intlLowerer().emitIntlOptions(at(1), at(2), held);
      llvm::Value *result = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_str_locale_compare", i32,
                                                          {ptr, i64, ptr, i64, ptr}),
          {this->lowering.stringLowerer().stringObj(self.value),
           this->lowering.stringLowerer().stringMeta(self.value),
           this->lowering.stringLowerer().stringObj(that.value),
           this->lowering.stringLowerer().stringMeta(that.value), options},
          "order");
      dropAll(held);
      this->lowering.refCounts().dropOperand(that);
      this->lowering.refCounts().dropOperand(self);
      return this->lowering.builder.CreateSExt(result, this->lowering.builder.getInt64Ty());
    }
    case Builtin::StrNormalize: {
      Lowering::Operand self = receiver(0);
      llvm::Value *has = this->lowering.builder.getInt32(0);
      llvm::Value *formObj = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
      llvm::Value *formMeta = this->lowering.builder.getInt64(0);
      std::optional<Lowering::Operand> form;
      if (!call->args.empty()) {
        form = this->lowering.refCounts().emitOperand(call->args[0], true);
        has = this->lowering.builder.getInt32(1);
        formObj = this->lowering.stringLowerer().stringObj(form->value);
        formMeta = this->lowering.stringLowerer().stringMeta(form->value);
      }
      llvm::Value *text =
          stringOf("tz_str_normalize",
                   {this->lowering.stringLowerer().stringObj(self.value),
                    this->lowering.stringLowerer().stringMeta(self.value), has, formObj, formMeta},
                   {ptr, i64, i32, ptr, i64});
      if (form) {
        this->lowering.refCounts().dropOperand(*form);
      }
      this->lowering.refCounts().dropOperand(self);
      return text;
    }
    case Builtin::RtfFormat:
    case Builtin::RtfFormatToParts: {
      Lowering::Operand self = receiver(0);
      llvm::Value *value = this->lowering.exprLowerer().emitExpr(call->args[0]);
      Lowering::Operand unit = this->lowering.refCounts().emitOperand(call->args[1], true);
      llvm::Value *result;
      if (call->builtin() == Builtin::RtfFormat) {
        result = stringOf("tz_rtf_format",
                          {self.value, value, this->lowering.stringLowerer().stringObj(unit.value),
                           this->lowering.stringLowerer().stringMeta(unit.value)},
                          {ptr, f64, ptr, i64});
      } else {
        result = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_rtf_format_to_parts", ptr,
                                                            {ptr, f64, ptr, i64, ptr}),
            {self.value, value, this->lowering.stringLowerer().stringObj(unit.value),
             this->lowering.stringLowerer().stringMeta(unit.value),
             this->lowering.intlLowerer().intlShape(call->type->inner())},
            "parts");
      }
      this->lowering.refCounts().dropOperand(unit);
      this->lowering.refCounts().dropOperand(self);
      return result;
    }
    case Builtin::LfFormat:
    case Builtin::LfFormatToParts: {
      Lowering::Operand self = receiver(0);
      Lowering::Operand list = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *result = call->builtin() == Builtin::LfFormat
                                ? stringOf("tz_lf_format", {self.value, list.value}, {ptr, ptr})
                                : this->lowering.builder.CreateCall(
                                      this->lowering.processLowerer().processFunction(
                                          "tz_lf_format_to_parts", ptr, {ptr, ptr, ptr}),
                                      {self.value, list.value,
                                       this->lowering.intlLowerer().intlShape(call->type->inner())},
                                      "parts");
      this->lowering.refCounts().dropOperand(list);
      this->lowering.refCounts().dropOperand(self);
      return result;
    }
    case Builtin::DnOf: {
      Lowering::Operand self = receiver(0);
      Lowering::Operand code = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::AllocaInst *out =
          this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "name.out");
      llvm::Value *has = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_dn_of", i32, {ptr, ptr, ptr, i64}),
          {out, self.value, this->lowering.stringLowerer().stringObj(code.value),
           this->lowering.stringLowerer().stringMeta(code.value)});
      this->lowering.refCounts().dropOperand(code);
      this->lowering.refCounts().dropOperand(self);
      return this->lowering.optionalLowerer().makeOptional(
          this->lowering.builder.CreateLoad(this->lowering.strTy, out),
          this->lowering.builder.CreateICmpNE(has, this->lowering.builder.getInt32(0)), call->type);
    }
    case Builtin::SegSegment: {
      Lowering::Operand self = receiver(0);
      Lowering::Operand text = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *segments = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_seg_segment", ptr, {ptr, ptr, i64}),
          {self.value, this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value)},
          "segments");
      this->lowering.refCounts().dropOperand(text);
      this->lowering.refCounts().dropOperand(self);
      return segments;
    }
    case Builtin::SegmentsArray:
    case Builtin::SegmentsContaining: {
      Lowering::Operand self = receiver(0);
      llvm::Value *result;
      if (call->builtin() == Builtin::SegmentsArray) {
        result = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_segments_array", ptr, {ptr, ptr}),
            {self.value, this->lowering.intlLowerer().intlShape(call->type->inner())}, "segments");
      } else {
        llvm::Value *index = call->args.empty()
                                 ? llvm::ConstantFP::get(f64, 0.0)
                                 : this->lowering.exprLowerer().emitExpr(call->args[0]);
        result = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_segments_containing", ptr,
                                                            {ptr, f64, ptr}),
            {self.value, index, this->lowering.intlLowerer().intlShape(call->type->nonOptional())},
            "segment");
      }
      this->lowering.refCounts().dropOperand(self);
      return result;
    }
    default:
      llvm_unreachable("not a built-in of Intl");
  }
}

}
