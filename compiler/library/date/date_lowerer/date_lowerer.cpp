#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/result/err_sites/err_sites.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
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

const llvm::StringLiteral kOptionNames[TZ_DTO_COUNT] = {
    "localeMatcher", "calendar", "numberingSystem", "hourCycle",     "timeZone",  "weekday",
    "era",           "year",     "month",           "day",           "dayPeriod", "hour",
    "minute",        "second",   "timeZoneName",    "formatMatcher", "dateStyle", "timeStyle",
};

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

}

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

DateLowerer::DateLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *DateLowerer::dateTimeAddress(llvm::Value *date) {
  return lowering_.objectLayouts().fieldAddress(date, Type::date(), "%time");
}

llvm::Value *DateLowerer::newDate(llvm::Value *time) {
  llvm::Value *date = lowering_.objectLayouts().allocObject(Type::date(), {});
  lowering_.builder_.CreateStore(time, dateTimeAddress(date));

  return date;
}

llvm::Value *DateLowerer::emitDateOptions(llvm::ArrayRef<Expr *> args,
                                          std::vector<Lowering::Operand> &held) {
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *f64 = lowering_.builder_.getDoubleTy();
  auto *valuesTy = llvm::ArrayType::get(lowering_.strTy_, TZ_DTO_COUNT);
  auto *optionsTy =
      llvm::StructType::get(lowering_.ctx_, {lowering_.strTy_, valuesTy, i32, i32, f64});
  llvm::AllocaInst *options = lowering_.functionFrame().createStackSlot(optionsTy, "date.options");
  lowering_.builder_.CreateStore(llvm::Constant::getNullValue(optionsTy), options);
  llvm::Value *present = lowering_.builder_.getInt32(0);
  llvm::Value *hour12 = llvm::ConstantInt::getSigned(i32, -1);
  llvm::Value *fractional = llvm::ConstantFP::getNaN(f64);
  auto split = [&](llvm::Value *value,
                   const Type *type) -> std::pair<llvm::Value *, llvm::Value *> {
    if (!type->isOptional()) {
      return {value, lowering_.builder_.getTrue()};
    }

    return {lowering_.optionalLowerer().optionalValue(value, type),
            lowering_.optionalLowerer().optionalHas(value, type)};
  };
  auto mark = [&](llvm::Value *has, unsigned bit) {
    present = lowering_.builder_.CreateOr(
        present, lowering_.builder_.CreateShl(lowering_.builder_.CreateZExt(has, i32),
                                              lowering_.builder_.getInt32(bit)));
  };
  if (!args.empty()) {
    if (args[0]->type->isUndefined()) {
      lowering_.stmtLowerer().emitDiscarded(args[0]);
    } else {
      Lowering::Operand locale = lowering_.refCounts().emitOperand(
          args[0], args.size() < 2 || AssignLowerer::cannotAssign(args[1]));
      held.push_back(locale);
      auto [value, has] = split(locale.value, locale.type);
      lowering_.builder_.CreateStore(value,
                                     lowering_.builder_.CreateStructGEP(optionsTy, options, 0));
      mark(has, 31);
    }
  }
  if (args.size() > 1) {
    if (args[1]->type->isUndefined()) {
      lowering_.stmtLowerer().emitDiscarded(args[1]);
    } else {
      Lowering::Operand object = lowering_.refCounts().emitOperand(args[1], true);
      held.push_back(object);
      for (const Field &field : object.type->fields()) {
        llvm::Value *loaded = lowering_.builder_.CreateLoad(
            lowering_.moduleBuilder().lower(field.type),
            lowering_.objectLayouts().fieldAddress(object.value, object.type, field.name),
            field.name);
        auto [value, has] = split(loaded, field.type);
        const Type *type = field.type->nonOptional();
        if (field.name == "hour12") {
          hour12 = lowering_.builder_.CreateSelect(has, lowering_.builder_.CreateZExt(value, i32),
                                                   hour12);
        } else if (field.name == "fractionalSecondDigits") {
          llvm::Value *digits =
              type->isFloat() ? value : lowering_.builder_.CreateSIToFP(value, f64);
          fractional = lowering_.builder_.CreateSelect(has, digits, fractional);
        } else {
          unsigned index =
              unsigned(llvm::find(kOptionNames, field.name) - std::begin(kOptionNames));
          if (!type->isString()) {
            value = lowering_.exprLowerer().emitConversion(value, type, Type::string());
          }
          lowering_.builder_.CreateStore(
              value,
              lowering_.builder_.CreateConstGEP2_32(
                  valuesTy, lowering_.builder_.CreateStructGEP(optionsTy, options, 1), 0, index));
          mark(has, index);
        }
      }
    }
  }
  for (Lowering::Operand &operand : held) {
  }
  lowering_.builder_.CreateStore(present,
                                 lowering_.builder_.CreateStructGEP(optionsTy, options, 2));
  lowering_.builder_.CreateStore(hour12, lowering_.builder_.CreateStructGEP(optionsTy, options, 3));
  lowering_.builder_.CreateStore(fractional,
                                 lowering_.builder_.CreateStructGEP(optionsTy, options, 4));

  return options;
}

llvm::Value *DateLowerer::parseResult(llvm::Value *time, llvm::Value *text, CallExpr *call) {
  static const std::vector<MakerFields> orders = {{nullptr, {"code", "message", "text"}}};
  const Type *type = call->type, *errorType = type->errType();
  llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("date.bad");
  llvm::BasicBlock *goodBlock = lowering_.functionFrame().newBlock("date.good");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("date.parsed");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateFCmpUNO(time, time), badBlock,
                                  goodBlock);
  lowering_.functionFrame().switchTo(badBlock);
  llvm::StructType *partsType = llvm::StructType::get(
      lowering_.ctx_, {lowering_.builder_.getPtrTy(), lowering_.strTy_, lowering_.strTy_});
  llvm::Value *parts = lowering_.functionFrame().createStackSlot(partsType, "date.error");
  lowering_.builder_.CreateStore(lowering_.stringLowerer().utf8Constant("bad_date"),
                                 lowering_.builder_.CreateStructGEP(partsType, parts, 0));
  lowering_.builder_.CreateStore(lowering_.stringLowerer().stringLiteral(u"Invalid Date"),
                                 lowering_.builder_.CreateStructGEP(partsType, parts, 1));
  lowering_.refCounts().retainValue(text, Type::string());
  lowering_.builder_.CreateStore(text, lowering_.builder_.CreateStructGEP(partsType, parts, 2));
  llvm::Value *error =
      lowering_.builder_.CreateCall(lowering_.objectMaker().maker(errorType, orders), {parts});
  llvm::Value *bad = lowering_.resultLowerer().makeResult(
      type, lowering_.builder_.getFalse(), nullptr, error, lowering_.errSites().site(call->loc()));
  llvm::BasicBlock *badEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(goodBlock);
  llvm::Value *good =
      lowering_.resultLowerer().makeResult(type, lowering_.builder_.getTrue(), time, nullptr);
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(good->getType(), 2, "date.result");
  phi->addIncoming(bad, badEnd);
  phi->addIncoming(good, goodBlock);

  return phi;
}

llvm::Value *DateLowerer::emitDateCall(CallExpr *call) {
  llvm::Type *voidTy = lowering_.builder_.getVoidTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  llvm::Type *f64 = lowering_.builder_.getDoubleTy(), *ptr = lowering_.builder_.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto constant = [&](size_t i) {
    return lowering_.builder_.getInt32(uint32_t(cast<IntLitExpr>(call->args[i])->value));
  };
  auto timeOf = [&](llvm::Value *date) {
    return lowering_.builder_.CreateLoad(f64, dateTimeAddress(date), "time");
  };
  auto receiver = [&](size_t from) {
    auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
    Lowering::Operand self = lowering_.refCounts().emitOperand(
        member->object, llvm::all_of(rest, AssignLowerer::cannotAssign));

    return self;
  };
  auto stringOf = [&](llvm::StringRef name, std::vector<llvm::Value *> args,
                      std::vector<llvm::Type *> params) {
    params.insert(params.begin(), ptr);

    return lowering_.stringLowerer().callReturningString(
        lowering_.processLowerer().processFunction(name, voidTy, params), args);
  };

  switch (call->builtin) {
  case Builtin::DateNew: {
    llvm::Value *time =
        call->args.empty()
            ? lowering_.builder_.CreateCall(
                  lowering_.processLowerer().processFunction("tz_date_now", f64, {}))
            : lowering_.builder_.CreateCall(
                  lowering_.processLowerer().processFunction("tz_date_clip", f64, {f64}),
                  {lowering_.exprLowerer().emitExpr(call->args[0])});
    return newDate(time);
  }
  case Builtin::DateCopy: {
    Lowering::Operand date = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *time = timeOf(date.value);
    lowering_.refCounts().dropOperand(date);
    return newDate(time);
  }
  case Builtin::DateFromString:
  case Builtin::DateParse: {
    Lowering::Operand text = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *time = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_date_parse", f64, {ptr, i64}),
        {lowering_.stringLowerer().stringObj(text.value),
         lowering_.stringLowerer().stringMeta(text.value)});
    if (call->builtin == Builtin::DateParse) {
      llvm::Value *result = parseResult(time, text.value, call);
      lowering_.refCounts().dropOperand(text);
      return result;
    }
    lowering_.refCounts().dropOperand(text);
    return newDate(time);
  }
  case Builtin::DateFromFields:
  case Builtin::DateUTC: {
    std::vector<llvm::Value *> values;
    for (Expr *arg : call->args) {
      values.push_back(lowering_.exprLowerer().emitExpr(arg));
    }
    for (size_t i = values.size(); i < 7; ++i) {
      values.push_back(llvm::ConstantFP::get(f64, i == 2 ? 1.0 : 0.0));
    }
    values.push_back(lowering_.builder_.getInt32(call->builtin == Builtin::DateFromFields));
    llvm::Value *time = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_date_from_fields", f64,
                                                   {f64, f64, f64, f64, f64, f64, f64, i32}),
        values);
    return call->builtin == Builtin::DateUTC ? time : newDate(time);
  }
  case Builtin::DateText: {
    llvm::Value *now = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_date_now", f64, {}));
    return stringOf("tz_date_local_string", {now, lowering_.builder_.getInt32(TZ_DATE_TEXT_FULL)},
                    {f64, i32});
  }
  case Builtin::DateGetTime:
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal: {
    Lowering::Operand date = receiver(call->args.size());
    llvm::Value *time = timeOf(date.value);
    lowering_.refCounts().dropOperand(date);
    if (call->builtin == Builtin::DateGetTime) {
      return time;
    }
    bool utc = call->builtin == Builtin::DateGetUTC;
    return lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction(
            utc ? "tz_date_utc_field" : "tz_date_local_field", f64, {f64, i32}),
        {time, constant(0)});
  }
  case Builtin::DateSetTime:
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal: {
    Lowering::Operand date = receiver(0);
    bool setTime = call->builtin == Builtin::DateSetTime;
    std::vector<llvm::Value *> values;
    for (size_t i = setTime ? 0 : 1; i < call->args.size(); ++i) {
      values.push_back(lowering_.exprLowerer().emitExpr(call->args[i]));
    }
    llvm::Value *address = dateTimeAddress(date.value);
    llvm::Value *time;
    if (setTime) {
      time = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_date_clip", f64, {f64}), {values[0]});
    } else {
      llvm::Value *count = lowering_.builder_.getInt32(uint32_t(values.size()));
      while (values.size() < 4) {
        values.push_back(llvm::ConstantFP::get(f64, 0.0));
      }
      time = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_date_set", f64,
                                                     {f64, i32, i32, i32, f64, f64, f64, f64}),
          {lowering_.builder_.CreateLoad(f64, address, "time"), constant(0),
           lowering_.builder_.getInt32(call->builtin == Builtin::DateSetLocal), count, values[0],
           values[1], values[2], values[3]});
    }
    lowering_.builder_.CreateStore(time, address);
    lowering_.refCounts().dropOperand(date);
    return time;
  }
  case Builtin::DateToISOString:
  case Builtin::DateToUTCString:
  case Builtin::DateToLocalString:
  case Builtin::DateToJSON: {
    Lowering::Operand date = receiver(0);
    if (call->builtin == Builtin::DateToJSON && !call->args.empty()) {
      lowering_.stmtLowerer().emitDiscarded(call->args[0]);
    }
    llvm::Value *time = timeOf(date.value);
    lowering_.refCounts().dropOperand(date);
    if (call->builtin == Builtin::DateToISOString) {
      llvm::Value *text = stringOf("tz_date_iso", {time}, {f64});
      return text;
    }
    if (call->builtin == Builtin::DateToUTCString) {
      return stringOf("tz_date_utc_string", {time}, {f64});
    }
    if (call->builtin == Builtin::DateToLocalString) {
      return stringOf("tz_date_local_string", {time, constant(0)}, {f64, i32});
    }
    llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "json.out");
    llvm::Value *has = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_date_json", i32, {ptr, f64}), {out, time});
    return lowering_.optionalLowerer().makeOptional(
        lowering_.builder_.CreateLoad(lowering_.strTy_, out),
        lowering_.builder_.CreateICmpNE(has, lowering_.builder_.getInt32(0)), call->type);
  }
  case Builtin::DateToLocale: {
    Lowering::Operand date = receiver(1);
    std::vector<Lowering::Operand> held;
    llvm::ArrayRef<Expr *> args = llvm::ArrayRef<Expr *>(call->args).drop_front();
    llvm::Value *options = emitDateOptions(args, held);
    llvm::Value *time = timeOf(date.value);
    llvm::Value *text = stringOf("tz_date_locale", {time, constant(0), options}, {f64, i32, ptr});
    for (const Lowering::Operand &operand : held) {
      lowering_.refCounts().dropOperand(operand);
    }
    lowering_.refCounts().dropOperand(date);
    return text;
  }
  case Builtin::DtfNew: {
    std::vector<Lowering::Operand> held;
    llvm::Value *options = emitDateOptions(call->args, held);
    llvm::Value *format = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_dtf_new", ptr, {ptr}), {options});
    for (const Lowering::Operand &operand : held) {
      lowering_.refCounts().dropOperand(operand);
    }
    return format;
  }
  case Builtin::DtfFormat:
  case Builtin::DtfFormatToParts: {
    Lowering::Operand format = receiver(0);
    llvm::Value *time = nullptr;
    if (call->args.empty()) {
      time = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_date_now", f64, {}));
    } else if (call->args[0]->type->isDate()) {
      Lowering::Operand date = lowering_.refCounts().emitOperand(call->args[0], true);
      time = timeOf(date.value);
      lowering_.refCounts().dropOperand(date);
    } else {
      time = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_date_clip", f64, {f64}),
          {lowering_.exprLowerer().emitExpr(call->args[0])});
    }
    llvm::Value *result;
    if (call->builtin == Builtin::DtfFormat) {
      result = stringOf("tz_dtf_format", {format.value, time}, {ptr, f64});
    } else {
      const Type *part = call->type->inner();
      const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(part);
      const llvm::StructLayout *offsets =
          lowering_.module_->getDataLayout().getStructLayout(layout.type);
      auto offsetOf = [&](llvm::StringRef name) {
        return lowering_.builder_.getInt32(uint32_t(
            offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(part, name)])));
      };
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_dtf_format_to_parts", ptr,
                                                     {ptr, f64, ptr, ptr, i64, i32, i32}),
          {format.value, time, lowering_.typeDescriptors().typeDescriptor(part),
           lowering_.objectLayouts().layoutDescriptor(part, {0, 1}),
           lowering_.builder_.getInt64(
               lowering_.module_->getDataLayout().getTypeAllocSize(layout.type)),
           offsetOf("type"), offsetOf("value")},
          "parts");
    }
    lowering_.refCounts().dropOperand(format);
    return result;
  }
  case Builtin::DtfResolvedOptions: {
    Lowering::Operand format = receiver(0);
    const Type *type = call->type;
    std::vector<unsigned> keys(type->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);
    llvm::Value *object = lowering_.objectLayouts().allocObject(type, keys);
    for (const Field &field : type->fields()) {
      llvm::Value *address = lowering_.objectLayouts().fieldAddress(object, type, field.name);
      llvm::Value *value;
      if (field.name == "hour12") {
        llvm::Value *v = lowering_.builder_.CreateCall(
            lowering_.processLowerer().processFunction("tz_dtf_hour12", i32, {ptr}),
            {format.value});
        value = lowering_.optionalLowerer().makeOptional(
            lowering_.builder_.CreateICmpEQ(v, lowering_.builder_.getInt32(1)),
            lowering_.builder_.CreateICmpSGE(v, lowering_.builder_.getInt32(0)), field.type);
      } else if (field.name == "fractionalSecondDigits") {
        llvm::Value *v = lowering_.builder_.CreateCall(
            lowering_.processLowerer().processFunction("tz_dtf_fractional", i32, {ptr}),
            {format.value});
        value = lowering_.optionalLowerer().makeOptional(
            v, lowering_.builder_.CreateICmpSGT(v, lowering_.builder_.getInt32(0)), field.type);
      } else {
        auto at = llvm::find(kOptionNames, field.name);
        int which = at == std::end(kOptionNames) ? -1 : int(at - std::begin(kOptionNames));
        llvm::AllocaInst *out =
            lowering_.functionFrame().createStackSlot(lowering_.strTy_, "option.out");
        llvm::Value *has = lowering_.builder_.CreateCall(
            lowering_.processLowerer().processFunction("tz_dtf_option", i32, {ptr, ptr, i32}),
            {out, format.value, lowering_.builder_.getInt32(uint32_t(which))});
        llvm::Value *text = lowering_.builder_.CreateLoad(lowering_.strTy_, out);
        value =
            field.type->isOptional()
                ? lowering_.optionalLowerer().makeOptional(
                      text, lowering_.builder_.CreateICmpNE(has, lowering_.builder_.getInt32(0)),
                      field.type)
                : text;
      }
      lowering_.builder_.CreateStore(value, address);
    }
    lowering_.refCounts().dropOperand(format);
    return object;
  }
  default:
    llvm_unreachable("not a built-in of Date");
  }
}

}
