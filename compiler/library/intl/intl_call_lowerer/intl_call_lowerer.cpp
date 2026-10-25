#include "compiler/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/library/intl/intl_lowerer/intl_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <numeric>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

const llvm::StringLiteral kOptionNames[TZ_IO_COUNT] = {
#define TZ_INTL_OPTION_NAME(id, name) name,
    TZ_INTL_OPTIONS(TZ_INTL_OPTION_NAME)
#undef TZ_INTL_OPTION_NAME
};

int optionIndex(llvm::StringRef name) {
  auto at = llvm::find(kOptionNames, name);

  return at == std::end(kOptionNames) ? -1 : int(at - std::begin(kOptionNames));
}

const Expr *withoutCasts(const Expr *e) {
  while (auto *cast = dyn_cast<CastExpr>(e)) {
    if (!cast->isImplicit()) {
      break;
    }
    e = cast->operand;
  }

  return e;
}

// Text known when compiling: a literal, or a template without values.
bool isLiteralText(const Expr *e) {
  e = withoutCasts(e);
  auto *tmpl = dyn_cast<TemplateExpr>(e);

  return llvm::isa<StringLitExpr>(e) || (tmpl && tmpl->exprs.empty());
}

bool isLiteralNumber(const Expr *e) {
  e = withoutCasts(e);
  if (auto *unary = dyn_cast<UnaryExpr>(e); unary && unary->op == UnaryOp::Neg) {
    e = withoutCasts(unary->operand);
  }

  return llvm::isa<IntLitExpr, FloatLitExpr>(e);
}

bool isLiteralValue(const Expr *e) {
  return isLiteralText(e) || isLiteralNumber(e) ||
         llvm::isa<BoolLitExpr, UndefinedExpr>(withoutCasts(e));
}

// The locale and the options (both may be left out), with all Checker could
// check written out: a locale that is a literal and an object literal of
// literals, without roundingIncrement, whose combinations only the running
// program checks.
bool optionsMayThrow(const Expr *locale, const Expr *options) {
  if (locale && !locale->type->isUndefined() && !isLiteralText(locale)) {
    return true;
  }
  if (!options || options->type->isUndefined()) {
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(withoutCasts(options));
  if (!literal) {
    return true;
  }

  return llvm::any_of(literal->props, [](const ObjectLitExpr::Property &prop) {
    return prop.spread || !prop.value || !isLiteralValue(prop.value) ||
           prop.name == "roundingIncrement";
  });
}

const Expr *argAt(const CallExpr *call, size_t i) {
  return i < call->args.size() ? call->args[i] : nullptr;
}

// The locales of getCanonicalLocales and supportedLocalesOf: a literal, or
// an array literal of them, Checker checked.
bool localesMayThrow(const Expr *arg) {
  if (!arg) {
    return false;
  }
  if (auto *array = dyn_cast<ArrayLitExpr>(withoutCasts(arg))) {
    return !llvm::all_of(array->elements, [](const ArrayLitExpr::Element &e) {
      return !e.spread && isLiteralText(e.expr);
    });
  }

  return !isLiteralValue(arg);
}

} // namespace

bool intlCallMayThrow(const CallExpr *call) {
  switch (call->builtin) {
  case Builtin::IntlNew: // the kind, then the locale and the options
    return optionsMayThrow(argAt(call, 1), argAt(call, 2));
  case Builtin::IntlSupportedLocales: // the name, the locales and the options
    return localesMayThrow(argAt(call, 1)) || optionsMayThrow(nullptr, argAt(call, 2));
  case Builtin::NumToLocale:
    return optionsMayThrow(argAt(call, 0), argAt(call, 1));
  case Builtin::StrLocaleCompare:
    return optionsMayThrow(argAt(call, 1), argAt(call, 2));
  case Builtin::IntlCanonicalLocales:
    return localesMayThrow(argAt(call, 0));
  case Builtin::StrNormalize:
    return !call->args.empty() && !isLiteralText(call->args[0]);
  case Builtin::RtfFormat:
  case Builtin::RtfFormatToParts:
    // A unit that is not one; a value that is NaN or infinite.
    return !isLiteralText(call->args[1]) || !isLiteralNumber(call->args[0]);
  case Builtin::DnOf:
    return true; // a code that is not one, which only the runtime knows
  default:
    return false;
  }
}

IntlCallLowerer::IntlCallLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *IntlCallLowerer::emitIntlCall(CallExpr *call) {
  llvm::Type *voidTy = lowering_.builder_.getVoidTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty(), *f64 = lowering_.builder_.getDoubleTy(),
             *ptr = lowering_.builder_.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  bool throws = intlCallMayThrow(call);
  // The receiver of a method, kept while the arguments from `from` on are
  // evaluated (held, if they may throw).
  auto receiver = [&](size_t from) {
    auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
    Lowering::Operand self = lowering_.refCounts().emitOperand(
        member->object, llvm::all_of(rest, AssignLowerer::cannotAssign));
    lowering_.throwLowerer().holdOperand(
        self, llvm::any_of(rest, [&](Expr *arg) { return lowering_.mayThrow().exprThrows(arg); }));

    return self;
  };
  auto stringOf = [&](llvm::StringRef name, std::vector<llvm::Value *> args,
                      std::vector<llvm::Type *> params) {
    params.insert(params.begin(), ptr);

    return lowering_.stringLowerer().callReturningString(
        lowering_.processLowerer().processFunction(name, voidTy, params), args);
  };
  auto at = [&](size_t i) -> Expr * { return i < call->args.size() ? call->args[i] : nullptr; };
  auto dropAll = [&](std::vector<Lowering::Operand> &held) {
    for (const Lowering::Operand &operand : held) {
      lowering_.refCounts().dropOperand(operand);
    }
  };

  switch (call->builtin) {
  case Builtin::IntlNew: {
    int kind = int(cast<IntLitExpr>(call->args[0])->value);
    std::vector<Lowering::Operand> held;
    llvm::Value *options = lowering_.intlLowerer().emitIntlOptions(at(1), at(2), held);
    llvm::Value *object = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_intl_new", ptr, {i32, ptr}),
        {lowering_.builder_.getInt32(uint32_t(kind)), options}, "intl");
    dropAll(held);
    lowering_.emitThrowCheckIf(throws);
    return object;
  }
  case Builtin::IntlSupportedLocales:
  case Builtin::IntlCanonicalLocales: {
    bool supported = call->builtin == Builtin::IntlSupportedLocales;
    llvm::Value *locales = lowering_.intlLowerer().emitLocaleArray(at(supported ? 1 : 0));
    llvm::Value *result;
    if (supported) {
      Lowering::Held heldLocales = lowering_.throwLowerer().hold(
          locales, Type::array(Type::string()), at(2) && lowering_.mayThrow().exprThrows(at(2)));
      std::vector<Lowering::Operand> held;
      llvm::Value *options = lowering_.intlLowerer().emitIntlOptions(nullptr, at(2), held);
      locales = lowering_.throwLowerer().take(heldLocales);
      const std::u16string &text = cast<StringLitExpr>(call->args[0])->value;
      std::string name;
      llvm::convertUTF16ToUTF8String(
          llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()),
                                      text.size()),
          name);
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_intl_supported_locales", ptr,
                                                     {ptr, ptr, ptr}),
          {locales, options, lowering_.stringLowerer().utf8Constant(name)}, "supported");
      dropAll(held);
    } else {
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_intl_canonical_locales", ptr, {ptr}),
          {locales}, "canonical");
    }
    lowering_.refCounts().releaseValue(locales, Type::array(Type::string()));
    lowering_.emitThrowCheckIf(throws);
    return result;
  }
  case Builtin::IntlResolvedOptions: {
    Lowering::Operand self = receiver(0);
    const Type *type = call->type;
    std::vector<unsigned> keys(type->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);
    llvm::Value *object = lowering_.objectLayouts().allocObject(type, keys);
    for (const Field &field : type->fields()) {
      llvm::Value *address = lowering_.objectLayouts().fieldAddress(object, type, field.name);
      const Type *inner = field.type->nonOptional();
      llvm::Value *value;
      if (field.name == "pluralCategories") {
        value = lowering_.builder_.CreateCall(
            lowering_.processLowerer().processFunction("tz_pr_categories", ptr, {ptr}),
            {self.value});
      } else if (field.name == "useGrouping") {
        // A string, or false.
        llvm::AllocaInst *out =
            lowering_.functionFrame().createStackSlot(lowering_.strTy_, "grouping.out");
        llvm::Value *on = lowering_.builder_.CreateICmpNE(
            lowering_.builder_.CreateCall(
                lowering_.processLowerer().processFunction("tz_intl_grouping", i32, {ptr, ptr}),
                {out, self.value}),
            lowering_.builder_.getInt32(0));
        llvm::Value *text = lowering_.unionLayout().makeTagged(
            field.type, UnionSlot::String, lowering_.builder_.CreateLoad(lowering_.strTy_, out));
        llvm::Value *off = lowering_.unionLayout().makeTagged(field.type, UnionSlot::Bool,
                                                              lowering_.builder_.getFalse());
        value = lowering_.builder_.CreateSelect(on, text, off);
      } else if (inner->isString()) {
        int which = field.name == "locale" ? -1 : optionIndex(field.name);
        llvm::AllocaInst *out =
            lowering_.functionFrame().createStackSlot(lowering_.strTy_, "option.out");
        llvm::Value *has = lowering_.builder_.CreateICmpNE(
            lowering_.builder_.CreateCall(
                lowering_.processLowerer().processFunction("tz_intl_text", i32, {ptr, ptr, i32}),
                {out, self.value, lowering_.builder_.getInt32(uint32_t(which))}),
            lowering_.builder_.getInt32(0));
        llvm::Value *text = lowering_.builder_.CreateLoad(lowering_.strTy_, out);
        value = field.type->isOptional()
                    ? lowering_.optionalLowerer().makeOptional(text, has, field.type)
                    : text;
      } else {
        llvm::Value *number = lowering_.builder_.CreateCall(
            lowering_.processLowerer().processFunction("tz_intl_number", f64, {ptr, i32}),
            {self.value, lowering_.builder_.getInt32(uint32_t(optionIndex(field.name)))});
        llvm::Value *has = lowering_.builder_.CreateFCmpORD(number, number);
        number = lowering_.builder_.CreateSelect(has, number, llvm::ConstantFP::get(f64, 0.0));
        llvm::Value *v =
            inner->isBool()
                ? lowering_.builder_.CreateFCmpUNE(number, llvm::ConstantFP::get(f64, 0.0))
                : lowering_.builder_.CreateFPToSI(number, lowering_.moduleBuilder().lower(inner));
        value = field.type->isOptional()
                    ? lowering_.optionalLowerer().makeOptional(v, has, field.type)
                    : v;
      }
      lowering_.builder_.CreateStore(value, address);
    }
    lowering_.refCounts().dropOperand(self);
    return object;
  }
  case Builtin::IntlBound: {
    IntlKind kind = member->object->type->intlKind();
    llvm::Value *object =
        lowering_.exprLowerer().emitExpr(member->object); // the environment's reference
    return lowering_.closureLowerer().makeFunctionValue(
        lowering_.intlLowerer().intlBoundInfo(kind, call->type), object);
  }
  case Builtin::NumToLocale: {
    Lowering::IntlNumber n = lowering_.intlLowerer().emitIntlNumber(member->object);
    lowering_.throwLowerer().holdOperand(n.operand,
                                         (at(0) && lowering_.mayThrow().exprThrows(at(0))) ||
                                             (at(1) && lowering_.mayThrow().exprThrows(at(1))));
    std::vector<Lowering::Operand> held;
    llvm::Value *options = lowering_.intlLowerer().emitIntlOptions(at(0), at(1), held);
    lowering_.throwLowerer().takeOperand(n.operand);
    std::vector<llvm::Value *> args = lowering_.intlLowerer().intlNumberArgs(n);
    args.push_back(options);
    llvm::Value *text = stringOf("tz_num_locale", args, {i32, f64, i64, i64, ptr});
    dropAll(held);
    lowering_.refCounts().dropOperand(n.operand);
    lowering_.emitThrowCheckIf(throws);
    return text;
  }
  case Builtin::NfFormat:
  case Builtin::NfFormatToParts:
  case Builtin::PrSelect: {
    Lowering::Operand self = receiver(0);
    Lowering::IntlNumber n = lowering_.intlLowerer().emitIntlNumber(call->args[0]);
    lowering_.throwLowerer().takeOperand(self);
    std::vector<llvm::Value *> args{self.value};
    for (llvm::Value *v : lowering_.intlLowerer().intlNumberArgs(n)) {
      args.push_back(v);
    }
    llvm::Value *result;
    if (call->builtin == Builtin::NfFormatToParts) {
      args.push_back(lowering_.intlLowerer().intlShape(call->type->inner()));
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_nf_format_to_parts", ptr,
                                                     {ptr, i32, f64, i64, i64, ptr}),
          args, "parts");
    } else {
      result = stringOf(call->builtin == Builtin::NfFormat ? "tz_nf_format" : "tz_pr_select", args,
                        {ptr, i32, f64, i64, i64});
    }
    lowering_.refCounts().dropOperand(n.operand);
    lowering_.refCounts().dropOperand(self);
    return result;
  }
  case Builtin::CollatorCompare: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand a = lowering_.refCounts().emitOperand(
        call->args[0], AssignLowerer::cannotAssign(call->args[1]));
    lowering_.throwLowerer().holdOperand(a, lowering_.mayThrow().exprThrows(call->args[1]));
    Lowering::Operand b = lowering_.refCounts().emitOperand(call->args[1], /*canBorrow=*/true);
    lowering_.throwLowerer().takeOperand(a);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *result =
        lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                          "tz_collator_compare", i32, {ptr, ptr, i64, ptr, i64}),
                                      {self.value, lowering_.stringLowerer().stringObj(a.value),
                                       lowering_.stringLowerer().stringMeta(a.value),
                                       lowering_.stringLowerer().stringObj(b.value),
                                       lowering_.stringLowerer().stringMeta(b.value)},
                                      "order");
    lowering_.refCounts().dropOperand(b);
    lowering_.refCounts().dropOperand(a);
    lowering_.refCounts().dropOperand(self);
    return result;
  }
  case Builtin::StrLocaleCompare: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand that = lowering_.refCounts().emitOperand(
        call->args[0],
        llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), AssignLowerer::cannotAssign));
    lowering_.throwLowerer().holdOperand(that,
                                         (at(1) && lowering_.mayThrow().exprThrows(at(1))) ||
                                             (at(2) && lowering_.mayThrow().exprThrows(at(2))));
    std::vector<Lowering::Operand> held;
    llvm::Value *options = lowering_.intlLowerer().emitIntlOptions(at(1), at(2), held);
    lowering_.throwLowerer().takeOperand(that);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *result =
        lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                          "tz_str_locale_compare", i32, {ptr, i64, ptr, i64, ptr}),
                                      {lowering_.stringLowerer().stringObj(self.value),
                                       lowering_.stringLowerer().stringMeta(self.value),
                                       lowering_.stringLowerer().stringObj(that.value),
                                       lowering_.stringLowerer().stringMeta(that.value), options},
                                      "order");
    dropAll(held);
    lowering_.refCounts().dropOperand(that);
    lowering_.refCounts().dropOperand(self);
    lowering_.emitThrowCheckIf(throws);
    return result;
  }
  case Builtin::StrNormalize: {
    Lowering::Operand self = receiver(0);
    llvm::Value *has = lowering_.builder_.getInt32(0);
    llvm::Value *formObj = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
    llvm::Value *formMeta = lowering_.builder_.getInt64(0);
    std::optional<Lowering::Operand> form;
    if (!call->args.empty()) {
      form = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      has = lowering_.builder_.getInt32(1);
      formObj = lowering_.stringLowerer().stringObj(form->value);
      formMeta = lowering_.stringLowerer().stringMeta(form->value);
    }
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *text =
        stringOf("tz_str_normalize",
                 {lowering_.stringLowerer().stringObj(self.value),
                  lowering_.stringLowerer().stringMeta(self.value), has, formObj, formMeta},
                 {ptr, i64, i32, ptr, i64});
    if (form) {
      lowering_.refCounts().dropOperand(*form);
    }
    lowering_.refCounts().dropOperand(self);
    lowering_.emitThrowCheckIf(throws);
    return text;
  }
  case Builtin::RtfFormat:
  case Builtin::RtfFormatToParts: {
    Lowering::Operand self = receiver(0);
    llvm::Value *value = lowering_.exprLowerer().emitExpr(call->args[0]);
    Lowering::Operand unit = lowering_.refCounts().emitOperand(call->args[1], /*canBorrow=*/true);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *result;
    if (call->builtin == Builtin::RtfFormat) {
      result = stringOf("tz_rtf_format",
                        {self.value, value, lowering_.stringLowerer().stringObj(unit.value),
                         lowering_.stringLowerer().stringMeta(unit.value)},
                        {ptr, f64, ptr, i64});
    } else {
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_rtf_format_to_parts", ptr,
                                                     {ptr, f64, ptr, i64, ptr}),
          {self.value, value, lowering_.stringLowerer().stringObj(unit.value),
           lowering_.stringLowerer().stringMeta(unit.value),
           lowering_.intlLowerer().intlShape(call->type->inner())},
          "parts");
    }
    lowering_.refCounts().dropOperand(unit);
    lowering_.refCounts().dropOperand(self);
    lowering_.emitThrowCheckIf(throws);
    return result;
  }
  case Builtin::LfFormat:
  case Builtin::LfFormatToParts: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand list = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *result =
        call->builtin == Builtin::LfFormat
            ? stringOf("tz_lf_format", {self.value, list.value}, {ptr, ptr})
            : lowering_.builder_.CreateCall(
                  lowering_.processLowerer().processFunction("tz_lf_format_to_parts", ptr,
                                                             {ptr, ptr, ptr}),
                  {self.value, list.value, lowering_.intlLowerer().intlShape(call->type->inner())},
                  "parts");
    lowering_.refCounts().dropOperand(list);
    lowering_.refCounts().dropOperand(self);
    return result;
  }
  case Builtin::DnOf: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand code = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    lowering_.throwLowerer().takeOperand(self);
    llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "name.out");
    llvm::Value *has = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_dn_of", i32, {ptr, ptr, ptr, i64}),
        {out, self.value, lowering_.stringLowerer().stringObj(code.value),
         lowering_.stringLowerer().stringMeta(code.value)});
    lowering_.refCounts().dropOperand(code);
    lowering_.refCounts().dropOperand(self);
    lowering_.emitThrowCheckIf(throws);
    return lowering_.optionalLowerer().makeOptional(
        lowering_.builder_.CreateLoad(lowering_.strTy_, out),
        lowering_.builder_.CreateICmpNE(has, lowering_.builder_.getInt32(0)), call->type);
  }
  case Builtin::SegSegment: {
    Lowering::Operand self = receiver(0);
    Lowering::Operand text = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    lowering_.throwLowerer().takeOperand(self);
    llvm::Value *segments = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_seg_segment", ptr, {ptr, ptr, i64}),
        {self.value, lowering_.stringLowerer().stringObj(text.value),
         lowering_.stringLowerer().stringMeta(text.value)},
        "segments");
    lowering_.refCounts().dropOperand(text);
    lowering_.refCounts().dropOperand(self);
    return segments;
  }
  case Builtin::SegmentsArray:
  case Builtin::SegmentsContaining: {
    Lowering::Operand self = receiver(0);
    llvm::Value *result;
    if (call->builtin == Builtin::SegmentsArray) {
      lowering_.throwLowerer().takeOperand(self);
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_segments_array", ptr, {ptr, ptr}),
          {self.value, lowering_.intlLowerer().intlShape(call->type->inner())}, "segments");
    } else {
      llvm::Value *index = call->args.empty() ? llvm::ConstantFP::get(f64, 0.0)
                                              : lowering_.exprLowerer().emitExpr(call->args[0]);
      lowering_.throwLowerer().takeOperand(self);
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_segments_containing", ptr,
                                                     {ptr, f64, ptr}),
          {self.value, index, lowering_.intlLowerer().intlShape(call->type->nonOptional())},
          "segment");
    }
    lowering_.refCounts().dropOperand(self);
    return result;
  }
  default:
    llvm_unreachable("not a built-in of Intl");
  }
}

} // namespace tz
