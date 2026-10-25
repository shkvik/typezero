// Dates (docs/date.md). A Date is an object { i64 rc, ptr layout, double
// time } that the code makes itself (TzDate in runtime/core/tzrt/tzrt.h):
// reading the time is a load, and `a - b` of dates two loads and a subtraction.
// The runtime computes with times (runtime/library/date/date/date.c): fields,
// setters, texts, parsing; local time and languages come from the ICU of the
// system (date_zone.c, date_format.c). An Intl.DateTimeFormat is an object the
// runtime makes, which holds a formatter of ICU.
//
// What JavaScript throws, the runtime throws (docs/errors.md): toISOString()
// and format() of an invalid date, a locale or options that only the
// running program shows to be wrong. The program checks tz_thrown after
// those calls, and ThrowScanner (codegen_throw.cpp) knows them.

#include "compiler/lower/codegen/codegen.h"

#include "runtime/library/date/date/date.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <cmath>
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;

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
  if (auto *cast = dyn_cast<CastExpr>(e); cast && cast->isImplicit())
    return isLiteralText(cast->operand);
  auto *tmpl = dyn_cast<TemplateExpr>(e);
  return llvm::isa<StringLitExpr>(e) || (tmpl && tmpl->exprs.empty());
}

bool isLiteralValue(const Expr *e) {
  if (auto *cast = dyn_cast<CastExpr>(e); cast && cast->isImplicit())
    return isLiteralValue(cast->operand);
  return isLiteralText(e) || llvm::isa<BoolLitExpr, IntLitExpr, FloatLitExpr, UndefinedExpr>(e);
}

} // namespace

// Sema checked what is written out (Sema::checkLocaleArgs); the time zone,
// the calendar and the numbering system only ICU knows, and a value that is
// not written out is known only when the program runs.
bool dateOptionsMayThrow(llvm::ArrayRef<Expr *> args) {
  if (!args.empty() && !args[0]->type->isUndefined() && !isLiteralText(args[0]))
    return true;
  if (args.size() < 2 || args[1]->type->isUndefined())
    return false;
  auto *literal = dyn_cast<ObjectLitExpr>(args[1]);
  if (!literal)
    return true;
  return llvm::any_of(literal->props, [](const ObjectLitExpr::Property &prop) {
    return prop.spread || !prop.value || !isLiteralValue(prop.value) || prop.name == "timeZone" ||
           prop.name == "calendar" || prop.name == "numberingSystem";
  });
}

llvm::Value *CodeGen::dateTimeAddress(llvm::Value *date) {
  return fieldAddress(date, Type::date(), "%time");
}

llvm::Value *CodeGen::newDate(llvm::Value *time) {
  llvm::Value *date = allocObject(Type::date(), {});
  builder_.CreateStore(time, dateTimeAddress(date));
  return date;
}

// TzDateOptions (runtime/core/tzrt/tzrt.h): { string locale, [TZ_DTO_COUNT x
// string] values, i32 present, i32 hour12, double fractional }. The strings are
// borrowed from the operands, which `held` keeps until after the call.
llvm::Value *CodeGen::emitDateOptions(llvm::ArrayRef<Expr *> args, std::vector<Operand> &held) {
  llvm::Type *i32 = builder_.getInt32Ty(), *f64 = builder_.getDoubleTy();
  auto *valuesTy = llvm::ArrayType::get(strTy_, TZ_DTO_COUNT);
  auto *optionsTy = llvm::StructType::get(ctx_, {strTy_, valuesTy, i32, i32, f64});
  llvm::AllocaInst *options = createStackSlot(optionsTy, "date.options");
  builder_.CreateStore(llvm::Constant::getNullValue(optionsTy), options);
  llvm::Value *present = builder_.getInt32(0);
  llvm::Value *hour12 = llvm::ConstantInt::getSigned(i32, -1);
  llvm::Value *fractional = llvm::ConstantFP::getNaN(f64);
  // A value and whether it is there, of a `T | undefined` or a T.
  auto split = [&](llvm::Value *value,
                   const Type *type) -> std::pair<llvm::Value *, llvm::Value *> {
    if (!type->isOptional())
      return {value, builder_.getTrue()};
    return {optionalValue(value, type), optionalHas(value, type)};
  };
  auto mark = [&](llvm::Value *has, unsigned bit) {
    present = builder_.CreateOr(
        present, builder_.CreateShl(builder_.CreateZExt(has, i32), builder_.getInt32(bit)));
  };
  if (!args.empty()) {
    if (args[0]->type->isUndefined()) {
      emitDiscarded(args[0]);
    } else {
      Operand locale = emitOperand(args[0], args.size() < 2 || cannotAssign(args[1]));
      holdOperand(locale, args.size() > 1 && exprThrows(args[1]));
      held.push_back(locale);
      auto [value, has] = split(locale.value, locale.type);
      builder_.CreateStore(value, builder_.CreateStructGEP(optionsTy, options, 0));
      mark(has, 31);
    }
  }
  if (args.size() > 1) {
    if (args[1]->type->isUndefined()) {
      emitDiscarded(args[1]);
    } else {
      Operand object = emitOperand(args[1], /*canBorrow=*/true);
      held.push_back(object);
      for (const Field &field : object.type->fields()) {
        llvm::Value *loaded = builder_.CreateLoad(
            lower(field.type), fieldAddress(object.value, object.type, field.name), field.name);
        auto [value, has] = split(loaded, field.type);
        const Type *type = field.type->nonOptional();
        if (field.name == "hour12") {
          hour12 = builder_.CreateSelect(has, builder_.CreateZExt(value, i32), hour12);
        } else if (field.name == "fractionalSecondDigits") {
          llvm::Value *digits = type->isFloat() ? value : builder_.CreateSIToFP(value, f64);
          fractional = builder_.CreateSelect(has, digits, fractional);
        } else {
          unsigned index =
              unsigned(llvm::find(kOptionNames, field.name) - std::begin(kOptionNames));
          builder_.CreateStore(
              value, builder_.CreateConstGEP2_32(
                         valuesTy, builder_.CreateStructGEP(optionsTy, options, 1), 0, index));
          mark(has, index);
        }
      }
    }
  }
  for (Operand &operand : held)
    takeOperand(operand);
  builder_.CreateStore(present, builder_.CreateStructGEP(optionsTy, options, 2));
  builder_.CreateStore(hour12, builder_.CreateStructGEP(optionsTy, options, 3));
  builder_.CreateStore(fractional, builder_.CreateStructGEP(optionsTy, options, 4));
  return options;
}

llvm::Value *CodeGen::emitDateCall(CallExpr *call) {
  llvm::Type *voidTy = builder_.getVoidTy(), *i32 = builder_.getInt32Ty(),
             *i64 = builder_.getInt64Ty();
  llvm::Type *f64 = builder_.getDoubleTy(), *ptr = builder_.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto constant = [&](size_t i) {
    return builder_.getInt32(uint32_t(cast<IntLitExpr>(call->args[i])->value));
  };
  // The time of a borrowed date.
  auto timeOf = [&](llvm::Value *date) {
    return builder_.CreateLoad(f64, dateTimeAddress(date), "time");
  };
  // The receiver of a method, kept while the arguments from `from` on are
  // evaluated (held, if they may throw: then takeOperand); its time is read
  // after them, as JavaScript reads it in the call.
  auto receiver = [&](size_t from) {
    auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
    Operand self = emitOperand(member->object, llvm::all_of(rest, cannotAssign));
    holdOperand(self, llvm::any_of(rest, [&](Expr *arg) { return exprThrows(arg); }));
    return self;
  };
  auto stringOf = [&](llvm::StringRef name, std::vector<llvm::Value *> args,
                      std::vector<llvm::Type *> params) {
    params.insert(params.begin(), ptr);
    return callReturningString(processFunction(name, voidTy, params), args);
  };

  switch (call->builtin) {
  case Builtin::DateNew: {
    llvm::Value *time = call->args.empty()
                            ? builder_.CreateCall(processFunction("tz_date_now", f64, {}))
                            : builder_.CreateCall(processFunction("tz_date_clip", f64, {f64}),
                                                  {emitExpr(call->args[0])});
    return newDate(time);
  }
  case Builtin::DateCopy: {
    Operand date = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *time = timeOf(date.value);
    dropOperand(date);
    return newDate(time);
  }
  case Builtin::DateFromString:
  case Builtin::DateParse: {
    Operand text = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *time = builder_.CreateCall(processFunction("tz_date_parse", f64, {ptr, i64}),
                                            {stringObj(text.value), stringMeta(text.value)});
    dropOperand(text);
    return call->builtin == Builtin::DateParse ? time : newDate(time);
  }
  case Builtin::DateFromFields:
  case Builtin::DateUTC: {
    // The parts left out: day 1, the rest 0.
    std::vector<llvm::Value *> values;
    for (Expr *arg : call->args)
      values.push_back(emitExpr(arg));
    for (size_t i = values.size(); i < 7; ++i)
      values.push_back(llvm::ConstantFP::get(f64, i == 2 ? 1.0 : 0.0));
    values.push_back(builder_.getInt32(call->builtin == Builtin::DateFromFields));
    llvm::Value *time = builder_.CreateCall(
        processFunction("tz_date_from_fields", f64, {f64, f64, f64, f64, f64, f64, f64, i32}),
        values);
    return call->builtin == Builtin::DateUTC ? time : newDate(time);
  }
  case Builtin::DateText: {
    llvm::Value *now = builder_.CreateCall(processFunction("tz_date_now", f64, {}));
    return stringOf("tz_date_local_string", {now, builder_.getInt32(TZ_DATE_TEXT_FULL)},
                    {f64, i32});
  }
  case Builtin::DateGetTime:
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal: {
    Operand date = receiver(call->args.size());
    llvm::Value *time = timeOf(date.value);
    dropOperand(date);
    if (call->builtin == Builtin::DateGetTime)
      return time;
    bool utc = call->builtin == Builtin::DateGetUTC;
    return builder_.CreateCall(
        processFunction(utc ? "tz_date_utc_field" : "tz_date_local_field", f64, {f64, i32}),
        {time, constant(0)});
  }
  case Builtin::DateSetTime:
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal: {
    Operand date = receiver(0);
    bool setTime = call->builtin == Builtin::DateSetTime;
    std::vector<llvm::Value *> values;
    for (size_t i = setTime ? 0 : 1; i < call->args.size(); ++i)
      values.push_back(emitExpr(call->args[i]));
    takeOperand(date);
    llvm::Value *address = dateTimeAddress(date.value);
    llvm::Value *time;
    if (setTime) {
      time = builder_.CreateCall(processFunction("tz_date_clip", f64, {f64}), {values[0]});
    } else {
      llvm::Value *count = builder_.getInt32(uint32_t(values.size()));
      while (values.size() < 4)
        values.push_back(llvm::ConstantFP::get(f64, 0.0));
      time = builder_.CreateCall(
          processFunction("tz_date_set", f64, {f64, i32, i32, i32, f64, f64, f64, f64}),
          {builder_.CreateLoad(f64, address, "time"), constant(0),
           builder_.getInt32(call->builtin == Builtin::DateSetLocal), count, values[0], values[1],
           values[2], values[3]});
    }
    builder_.CreateStore(time, address);
    dropOperand(date);
    return time;
  }
  case Builtin::DateToISOString:
  case Builtin::DateToUTCString:
  case Builtin::DateToLocalString:
  case Builtin::DateToJSON: {
    Operand date = receiver(0);
    if (call->builtin == Builtin::DateToJSON && !call->args.empty())
      emitDiscarded(call->args[0]); // the key, which a date ignores
    takeOperand(date);
    llvm::Value *time = timeOf(date.value);
    dropOperand(date);
    if (call->builtin == Builtin::DateToISOString) {
      llvm::Value *text = stringOf("tz_date_iso", {time}, {f64});
      emitThrowCheck(); // RangeError: Invalid time value
      return text;
    }
    if (call->builtin == Builtin::DateToUTCString)
      return stringOf("tz_date_utc_string", {time}, {f64});
    if (call->builtin == Builtin::DateToLocalString)
      return stringOf("tz_date_local_string", {time, constant(0)}, {f64, i32});
    llvm::AllocaInst *out = createStackSlot(strTy_, "json.out");
    llvm::Value *has =
        builder_.CreateCall(processFunction("tz_date_json", i32, {ptr, f64}), {out, time});
    return makeOptional(builder_.CreateLoad(strTy_, out),
                        builder_.CreateICmpNE(has, builder_.getInt32(0)), call->type);
  }
  case Builtin::DateToLocale: {
    Operand date = receiver(1);
    std::vector<Operand> held;
    llvm::ArrayRef<Expr *> args = llvm::ArrayRef<Expr *>(call->args).drop_front();
    llvm::Value *options = emitDateOptions(args, held);
    takeOperand(date);
    llvm::Value *time = timeOf(date.value);
    llvm::Value *text = stringOf("tz_date_locale", {time, constant(0), options}, {f64, i32, ptr});
    for (const Operand &operand : held)
      dropOperand(operand);
    dropOperand(date);
    emitThrowCheckIf(dateOptionsMayThrow(args));
    return text;
  }
  case Builtin::DtfNew: {
    std::vector<Operand> held;
    llvm::Value *options = emitDateOptions(call->args, held);
    llvm::Value *format = builder_.CreateCall(processFunction("tz_dtf_new", ptr, {ptr}), {options});
    for (const Operand &operand : held)
      dropOperand(operand);
    emitThrowCheckIf(dateOptionsMayThrow(call->args));
    return format;
  }
  case Builtin::DtfFormat:
  case Builtin::DtfFormatToParts: {
    Operand format = receiver(0);
    // A date, a time in milliseconds, or now.
    llvm::Value *time = nullptr;
    if (call->args.empty()) {
      time = builder_.CreateCall(processFunction("tz_date_now", f64, {}));
    } else if (call->args[0]->type->isDate()) {
      Operand date = emitOperand(call->args[0], /*canBorrow=*/true);
      time = timeOf(date.value);
      dropOperand(date);
    } else {
      time = builder_.CreateCall(processFunction("tz_date_clip", f64, {f64}),
                                 {emitExpr(call->args[0])});
    }
    takeOperand(format);
    llvm::Value *result;
    if (call->builtin == Builtin::DtfFormat) {
      result = stringOf("tz_dtf_format", {format.value, time}, {ptr, f64});
    } else {
      // Each part is a new object { type, value }.
      const Type *part = call->type->inner();
      const ObjectLayout &layout = objectLayout(part);
      const llvm::StructLayout *offsets = module_->getDataLayout().getStructLayout(layout.type);
      auto offsetOf = [&](llvm::StringRef name) {
        return builder_.getInt32(
            uint32_t(offsets->getElementOffset(layout.elements[fieldIndex(part, name)])));
      };
      result = builder_.CreateCall(
          processFunction("tz_dtf_format_to_parts", ptr, {ptr, f64, ptr, ptr, i64, i32, i32}),
          {format.value, time, typeDescriptor(part), layoutDescriptor(part, {0, 1}),
           builder_.getInt64(module_->getDataLayout().getTypeAllocSize(layout.type)),
           offsetOf("type"), offsetOf("value")},
          "parts");
    }
    dropOperand(format);
    emitThrowCheckIf(!call->args.empty()); // RangeError: Invalid time value
    return result;
  }
  case Builtin::DtfResolvedOptions: {
    Operand format = receiver(0);
    const Type *type = call->type;
    std::vector<unsigned> keys(type->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);
    llvm::Value *object = allocObject(type, keys);
    for (const Field &field : type->fields()) {
      llvm::Value *address = fieldAddress(object, type, field.name);
      llvm::Value *value;
      if (field.name == "hour12") {
        llvm::Value *v =
            builder_.CreateCall(processFunction("tz_dtf_hour12", i32, {ptr}), {format.value});
        value = makeOptional(builder_.CreateICmpEQ(v, builder_.getInt32(1)),
                             builder_.CreateICmpSGE(v, builder_.getInt32(0)), field.type);
      } else if (field.name == "fractionalSecondDigits") {
        llvm::Value *v =
            builder_.CreateCall(processFunction("tz_dtf_fractional", i32, {ptr}), {format.value});
        value = makeOptional(v, builder_.CreateICmpSGT(v, builder_.getInt32(0)), field.type);
      } else {
        // The locale is -1, the others the options of the same names.
        auto at = llvm::find(kOptionNames, field.name);
        int which = at == std::end(kOptionNames) ? -1 : int(at - std::begin(kOptionNames));
        llvm::AllocaInst *out = createStackSlot(strTy_, "option.out");
        llvm::Value *has =
            builder_.CreateCall(processFunction("tz_dtf_option", i32, {ptr, ptr, i32}),
                                {out, format.value, builder_.getInt32(uint32_t(which))});
        llvm::Value *text = builder_.CreateLoad(strTy_, out);
        value =
            field.type->isOptional()
                ? makeOptional(text, builder_.CreateICmpNE(has, builder_.getInt32(0)), field.type)
                : text;
      }
      builder_.CreateStore(value, address);
    }
    dropOperand(format);
    return object;
  }
  default:
    llvm_unreachable("not a built-in of Date");
  }
}

} // namespace tz
