#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The names of the options that are strings, by TZ_DTO_*, as Sema checked
// them (sema_date.cpp).
const llvm::StringLiteral kOptionNames[TZ_DTO_COUNT] = {
    "localeMatcher", "calendar", "numberingSystem", "hourCycle",     "timeZone",  "weekday",
    "era",           "year",     "month",           "day",           "dayPeriod", "hour",
    "minute",        "second",   "timeZoneName",    "formatMatcher", "dateStyle", "timeStyle",
};

// Text known when compiling: a literal, or a template without values.
bool isLiteralText(const Expr *e) {
  if (auto *cast = dyn_cast<CastExpr>(e); cast && cast->isImplicit()) {
    return isLiteralText(cast->operand);
  }
  auto *tmpl = dyn_cast<TemplateExpr>(e);

  return llvm::isa<StringLitExpr>(e) || (tmpl && tmpl->exprs.empty());
}

bool isLiteralValue(const Expr *e) {
  if (auto *cast = dyn_cast<CastExpr>(e); cast && cast->isImplicit()) {
    return isLiteralValue(cast->operand);
  }

  return isLiteralText(e) || llvm::isa<BoolLitExpr, IntLitExpr, FloatLitExpr, UndefinedExpr>(e);
}

} // namespace

// Sema checked what is written out (Sema::checkLocaleArgs); the time zone,
// the calendar and the numbering system only ICU knows, and a value that is
// not written out is known only when the program runs.
bool dateOptionsMayThrow(llvm::ArrayRef<Expr *> args) {
  if (!args.empty() && !args[0]->type->isUndefined() && !isLiteralText(args[0])) {
    return true;
  }
  if (args.size() < 2 || args[1]->type->isUndefined()) {
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(args[1]);
  if (!literal) {
    return true;
  }

  return llvm::any_of(literal->props, [](const ObjectLitExpr::Property &prop) {
    return prop.spread || !prop.value || !isLiteralValue(prop.value) || prop.name == "timeZone" ||
           prop.name == "calendar" || prop.name == "numberingSystem";
  });
}

DateLowerer::DateLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *DateLowerer::dateTimeAddress(llvm::Value *date) {
  return codegen_.objectLayouts().fieldAddress(date, Type::date(), "%time");
}

llvm::Value *DateLowerer::newDate(llvm::Value *time) {
  llvm::Value *date = codegen_.objectLayouts().allocObject(Type::date(), {});
  codegen_.builder_.CreateStore(time, dateTimeAddress(date));

  return date;
}

// TzDateOptions (runtime/core/tzrt/tzrt.h): { string locale, [TZ_DTO_COUNT x
// string] values, i32 present, i32 hour12, double fractional }. The strings are
// borrowed from the operands, which `held` keeps until after the call.
llvm::Value *DateLowerer::emitDateOptions(llvm::ArrayRef<Expr *> args, std::vector<CodeGen::Operand> &held) {
  llvm::Type *i32 = codegen_.builder_.getInt32Ty(), *f64 = codegen_.builder_.getDoubleTy();
  auto *valuesTy = llvm::ArrayType::get(codegen_.strTy_, TZ_DTO_COUNT);
  auto *optionsTy = llvm::StructType::get(codegen_.ctx_, {codegen_.strTy_, valuesTy, i32, i32, f64});
  llvm::AllocaInst *options = codegen_.functionFrame().createStackSlot(optionsTy, "date.options");
  codegen_.builder_.CreateStore(llvm::Constant::getNullValue(optionsTy), options);
  llvm::Value *present = codegen_.builder_.getInt32(0);
  llvm::Value *hour12 = llvm::ConstantInt::getSigned(i32, -1);
  llvm::Value *fractional = llvm::ConstantFP::getNaN(f64);
  // A value and whether it is there, of a `T | undefined` or a T.
  auto split = [&](llvm::Value *value,
                   const Type *type) -> std::pair<llvm::Value *, llvm::Value *> {
    if (!type->isOptional()) {
      return {value, codegen_.builder_.getTrue()};
    }

    return {codegen_.optionalLowerer().optionalValue(value, type), codegen_.optionalLowerer().optionalHas(value, type)};
  };
  auto mark = [&](llvm::Value *has, unsigned bit) {
    present = codegen_.builder_.CreateOr(
        present, codegen_.builder_.CreateShl(codegen_.builder_.CreateZExt(has, i32), codegen_.builder_.getInt32(bit)));
  };
  if (!args.empty()) {
    if (args[0]->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(args[0]);
    } else {
      CodeGen::Operand locale = codegen_.refCounts().emitOperand(args[0], args.size() < 2 || AssignLowerer::cannotAssign(args[1]));
      codegen_.throwLowerer().holdOperand(locale, args.size() > 1 && codegen_.mayThrow().exprThrows(args[1]));
      held.push_back(locale);
      auto [value, has] = split(locale.value, locale.type);
      codegen_.builder_.CreateStore(value, codegen_.builder_.CreateStructGEP(optionsTy, options, 0));
      mark(has, 31);
    }
  }
  if (args.size() > 1) {
    if (args[1]->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(args[1]);
    } else {
      CodeGen::Operand object = codegen_.refCounts().emitOperand(args[1], /*canBorrow=*/true);
      held.push_back(object);
      for (const Field &field : object.type->fields()) {
        llvm::Value *loaded = codegen_.builder_.CreateLoad(
            codegen_.moduleBuilder().lower(field.type), codegen_.objectLayouts().fieldAddress(object.value, object.type, field.name), field.name);
        auto [value, has] = split(loaded, field.type);
        const Type *type = field.type->nonOptional();
        if (field.name == "hour12") {
          hour12 = codegen_.builder_.CreateSelect(has, codegen_.builder_.CreateZExt(value, i32), hour12);
        } else if (field.name == "fractionalSecondDigits") {
          llvm::Value *digits = type->isFloat() ? value : codegen_.builder_.CreateSIToFP(value, f64);
          fractional = codegen_.builder_.CreateSelect(has, digits, fractional);
        } else {
          unsigned index =
              unsigned(llvm::find(kOptionNames, field.name) - std::begin(kOptionNames));
          codegen_.builder_.CreateStore(
              value, codegen_.builder_.CreateConstGEP2_32(
                         valuesTy, codegen_.builder_.CreateStructGEP(optionsTy, options, 1), 0, index));
          mark(has, index);
        }
      }
    }
  }
  for (CodeGen::Operand &operand : held) {
    codegen_.throwLowerer().takeOperand(operand);
  }
  codegen_.builder_.CreateStore(present, codegen_.builder_.CreateStructGEP(optionsTy, options, 2));
  codegen_.builder_.CreateStore(hour12, codegen_.builder_.CreateStructGEP(optionsTy, options, 3));
  codegen_.builder_.CreateStore(fractional, codegen_.builder_.CreateStructGEP(optionsTy, options, 4));

  return options;
}

llvm::Value *DateLowerer::emitDateCall(CallExpr *call) {
  llvm::Type *voidTy = codegen_.builder_.getVoidTy(), *i32 = codegen_.builder_.getInt32Ty(),
             *i64 = codegen_.builder_.getInt64Ty();
  llvm::Type *f64 = codegen_.builder_.getDoubleTy(), *ptr = codegen_.builder_.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto constant = [&](size_t i) {
    return codegen_.builder_.getInt32(uint32_t(cast<IntLitExpr>(call->args[i])->value));
  };
  // The time of a borrowed date.
  auto timeOf = [&](llvm::Value *date) {
    return codegen_.builder_.CreateLoad(f64, dateTimeAddress(date), "time");
  };
  // The receiver of a method, kept while the arguments from `from` on are
  // evaluated (held, if they may throw: then takeOperand); its time is read
  // after them, as JavaScript reads it in the call.
  auto receiver = [&](size_t from) {
    auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
    CodeGen::Operand self = codegen_.refCounts().emitOperand(member->object, llvm::all_of(rest, AssignLowerer::cannotAssign));
    codegen_.throwLowerer().holdOperand(self, llvm::any_of(rest, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));

    return self;
  };
  auto stringOf = [&](llvm::StringRef name, std::vector<llvm::Value *> args,
                      std::vector<llvm::Type *> params) {
    params.insert(params.begin(), ptr);

    return codegen_.stringLowerer().callReturningString(codegen_.processLowerer().processFunction(name, voidTy, params), args);
  };

  switch (call->builtin) {
  case Builtin::DateNew: {
    llvm::Value *time = call->args.empty()
                            ? codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_now", f64, {}))
                            : codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_clip", f64, {f64}),
                                                  {codegen_.exprLowerer().emitExpr(call->args[0])});
    return newDate(time);
  }
  case Builtin::DateCopy: {
    CodeGen::Operand date = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *time = timeOf(date.value);
    codegen_.refCounts().dropOperand(date);
    return newDate(time);
  }
  case Builtin::DateFromString:
  case Builtin::DateParse: {
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *time = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_parse", f64, {ptr, i64}),
                                            {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value)});
    codegen_.refCounts().dropOperand(text);
    return call->builtin == Builtin::DateParse ? time : newDate(time);
  }
  case Builtin::DateFromFields:
  case Builtin::DateUTC: {
    // The parts left out: day 1, the rest 0.
    std::vector<llvm::Value *> values;
    for (Expr *arg : call->args) {
      values.push_back(codegen_.exprLowerer().emitExpr(arg));
    }
    for (size_t i = values.size(); i < 7; ++i) {
      values.push_back(llvm::ConstantFP::get(f64, i == 2 ? 1.0 : 0.0));
    }
    values.push_back(codegen_.builder_.getInt32(call->builtin == Builtin::DateFromFields));
    llvm::Value *time = codegen_.builder_.CreateCall(
        codegen_.processLowerer().processFunction("tz_date_from_fields", f64, {f64, f64, f64, f64, f64, f64, f64, i32}),
        values);
    return call->builtin == Builtin::DateUTC ? time : newDate(time);
  }
  case Builtin::DateText: {
    llvm::Value *now = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_now", f64, {}));
    return stringOf("tz_date_local_string", {now, codegen_.builder_.getInt32(TZ_DATE_TEXT_FULL)},
                    {f64, i32});
  }
  case Builtin::DateGetTime:
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal: {
    CodeGen::Operand date = receiver(call->args.size());
    llvm::Value *time = timeOf(date.value);
    codegen_.refCounts().dropOperand(date);
    if (call->builtin == Builtin::DateGetTime) {
      return time;
    }
    bool utc = call->builtin == Builtin::DateGetUTC;
    return codegen_.builder_.CreateCall(
        codegen_.processLowerer().processFunction(utc ? "tz_date_utc_field" : "tz_date_local_field", f64, {f64, i32}),
        {time, constant(0)});
  }
  case Builtin::DateSetTime:
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal: {
    CodeGen::Operand date = receiver(0);
    bool setTime = call->builtin == Builtin::DateSetTime;
    std::vector<llvm::Value *> values;
    for (size_t i = setTime ? 0 : 1; i < call->args.size(); ++i) {
      values.push_back(codegen_.exprLowerer().emitExpr(call->args[i]));
    }
    codegen_.throwLowerer().takeOperand(date);
    llvm::Value *address = dateTimeAddress(date.value);
    llvm::Value *time;
    if (setTime) {
      time = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_clip", f64, {f64}), {values[0]});
    } else {
      llvm::Value *count = codegen_.builder_.getInt32(uint32_t(values.size()));
      while (values.size() < 4) {
        values.push_back(llvm::ConstantFP::get(f64, 0.0));
      }
      time = codegen_.builder_.CreateCall(
          codegen_.processLowerer().processFunction("tz_date_set", f64, {f64, i32, i32, i32, f64, f64, f64, f64}),
          {codegen_.builder_.CreateLoad(f64, address, "time"), constant(0),
           codegen_.builder_.getInt32(call->builtin == Builtin::DateSetLocal), count, values[0], values[1],
           values[2], values[3]});
    }
    codegen_.builder_.CreateStore(time, address);
    codegen_.refCounts().dropOperand(date);
    return time;
  }
  case Builtin::DateToISOString:
  case Builtin::DateToUTCString:
  case Builtin::DateToLocalString:
  case Builtin::DateToJSON: {
    CodeGen::Operand date = receiver(0);
    if (call->builtin == Builtin::DateToJSON && !call->args.empty()) {
      codegen_.stmtLowerer().emitDiscarded(call->args[0]); // the key, which a date ignores
    }
    codegen_.throwLowerer().takeOperand(date);
    llvm::Value *time = timeOf(date.value);
    codegen_.refCounts().dropOperand(date);
    if (call->builtin == Builtin::DateToISOString) {
      llvm::Value *text = stringOf("tz_date_iso", {time}, {f64});
      codegen_.throwLowerer().emitThrowCheck(); // RangeError: Invalid time value
      return text;
    }
    if (call->builtin == Builtin::DateToUTCString) {
      return stringOf("tz_date_utc_string", {time}, {f64});
    }
    if (call->builtin == Builtin::DateToLocalString) {
      return stringOf("tz_date_local_string", {time, constant(0)}, {f64, i32});
    }
    llvm::AllocaInst *out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "json.out");
    llvm::Value *has =
        codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_json", i32, {ptr, f64}), {out, time});
    return codegen_.optionalLowerer().makeOptional(codegen_.builder_.CreateLoad(codegen_.strTy_, out),
                        codegen_.builder_.CreateICmpNE(has, codegen_.builder_.getInt32(0)), call->type);
  }
  case Builtin::DateToLocale: {
    CodeGen::Operand date = receiver(1);
    std::vector<CodeGen::Operand> held;
    llvm::ArrayRef<Expr *> args = llvm::ArrayRef<Expr *>(call->args).drop_front();
    llvm::Value *options = emitDateOptions(args, held);
    codegen_.throwLowerer().takeOperand(date);
    llvm::Value *time = timeOf(date.value);
    llvm::Value *text = stringOf("tz_date_locale", {time, constant(0), options}, {f64, i32, ptr});
    for (const CodeGen::Operand &operand : held) {
      codegen_.refCounts().dropOperand(operand);
    }
    codegen_.refCounts().dropOperand(date);
    codegen_.emitThrowCheckIf(dateOptionsMayThrow(args));
    return text;
  }
  case Builtin::DtfNew: {
    std::vector<CodeGen::Operand> held;
    llvm::Value *options = emitDateOptions(call->args, held);
    llvm::Value *format = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_dtf_new", ptr, {ptr}), {options});
    for (const CodeGen::Operand &operand : held) {
      codegen_.refCounts().dropOperand(operand);
    }
    codegen_.emitThrowCheckIf(dateOptionsMayThrow(call->args));
    return format;
  }
  case Builtin::DtfFormat:
  case Builtin::DtfFormatToParts: {
    CodeGen::Operand format = receiver(0);
    // A date, a time in milliseconds, or now.
    llvm::Value *time = nullptr;
    if (call->args.empty()) {
      time = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_now", f64, {}));
    } else if (call->args[0]->type->isDate()) {
      CodeGen::Operand date = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      time = timeOf(date.value);
      codegen_.refCounts().dropOperand(date);
    } else {
      time = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_clip", f64, {f64}),
                                 {codegen_.exprLowerer().emitExpr(call->args[0])});
    }
    codegen_.throwLowerer().takeOperand(format);
    llvm::Value *result;
    if (call->builtin == Builtin::DtfFormat) {
      result = stringOf("tz_dtf_format", {format.value, time}, {ptr, f64});
    } else {
      // Each part is a new object { type, value }.
      const Type *part = call->type->inner();
      const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(part);
      const llvm::StructLayout *offsets = codegen_.module_->getDataLayout().getStructLayout(layout.type);
      auto offsetOf = [&](llvm::StringRef name) {
        return codegen_.builder_.getInt32(
            uint32_t(offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(part, name)])));
      };
      result = codegen_.builder_.CreateCall(
          codegen_.processLowerer().processFunction("tz_dtf_format_to_parts", ptr, {ptr, f64, ptr, ptr, i64, i32, i32}),
          {format.value, time, codegen_.typeDescriptors().typeDescriptor(part), codegen_.objectLayouts().layoutDescriptor(part, {0, 1}),
           codegen_.builder_.getInt64(codegen_.module_->getDataLayout().getTypeAllocSize(layout.type)),
           offsetOf("type"), offsetOf("value")},
          "parts");
    }
    codegen_.refCounts().dropOperand(format);
    codegen_.emitThrowCheckIf(!call->args.empty()); // RangeError: Invalid time value
    return result;
  }
  case Builtin::DtfResolvedOptions: {
    CodeGen::Operand format = receiver(0);
    const Type *type = call->type;
    std::vector<unsigned> keys(type->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);
    llvm::Value *object = codegen_.objectLayouts().allocObject(type, keys);
    for (const Field &field : type->fields()) {
      llvm::Value *address = codegen_.objectLayouts().fieldAddress(object, type, field.name);
      llvm::Value *value;
      if (field.name == "hour12") {
        llvm::Value *v =
            codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_dtf_hour12", i32, {ptr}), {format.value});
        value = codegen_.optionalLowerer().makeOptional(codegen_.builder_.CreateICmpEQ(v, codegen_.builder_.getInt32(1)),
                             codegen_.builder_.CreateICmpSGE(v, codegen_.builder_.getInt32(0)), field.type);
      } else if (field.name == "fractionalSecondDigits") {
        llvm::Value *v =
            codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_dtf_fractional", i32, {ptr}), {format.value});
        value = codegen_.optionalLowerer().makeOptional(v, codegen_.builder_.CreateICmpSGT(v, codegen_.builder_.getInt32(0)), field.type);
      } else {
        // The locale is -1, the others the options of the same names.
        auto at = llvm::find(kOptionNames, field.name);
        int which = at == std::end(kOptionNames) ? -1 : int(at - std::begin(kOptionNames));
        llvm::AllocaInst *out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "option.out");
        llvm::Value *has =
            codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_dtf_option", i32, {ptr, ptr, i32}),
                                {out, format.value, codegen_.builder_.getInt32(uint32_t(which))});
        llvm::Value *text = codegen_.builder_.CreateLoad(codegen_.strTy_, out);
        value =
            field.type->isOptional()
                ? codegen_.optionalLowerer().makeOptional(text, codegen_.builder_.CreateICmpNE(has, codegen_.builder_.getInt32(0)), field.type)
                : text;
      }
      codegen_.builder_.CreateStore(value, address);
    }
    codegen_.refCounts().dropOperand(format);
    return object;
  }
  default:
    llvm_unreachable("not a built-in of Date");
  }
}

} // namespace tz
