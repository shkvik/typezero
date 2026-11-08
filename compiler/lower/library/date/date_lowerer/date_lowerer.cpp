#include "compiler/lower/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/result/err_sites/err_sites.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

const llvm::StringLiteral kOptionNames[TZ_DTO_COUNT] = {
    "localeMatcher", "calendar", "numberingSystem", "hourCycle",     "timeZone",  "weekday",
    "era",           "year",     "month",           "day",           "dayPeriod", "hour",
    "minute",        "second",   "timeZoneName",    "formatMatcher", "dateStyle", "timeStyle",
};

}

DateLowerer::DateLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *DateLowerer::dateTimeAddress(llvm::Value *date) {
  return this->lowering.objectLayouts().fieldAddress(date, Type::date(), "%time");
}

llvm::Value *DateLowerer::newDate(llvm::Value *time) {
  llvm::Value *date = this->lowering.objectLayouts().allocObject(Type::date(), {});
  this->lowering.builder.CreateStore(time, this->dateTimeAddress(date));

  return date;
}

llvm::Value *DateLowerer::emitDateOptions(llvm::ArrayRef<Expr *> args,
                                          std::vector<Lowering::Operand> &held) {
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();
  auto *valuesTy = llvm::ArrayType::get(this->lowering.strTy, TZ_DTO_COUNT);
  auto *optionsTy =
      llvm::StructType::get(this->lowering.ctx, {this->lowering.strTy, valuesTy, i32, i32, f64});
  llvm::AllocaInst *options =
      this->lowering.functionFrame().createStackSlot(optionsTy, "date.options");
  this->lowering.builder.CreateStore(llvm::Constant::getNullValue(optionsTy), options);
  llvm::Value *present = this->lowering.builder.getInt32(0);
  llvm::Value *hour12 = llvm::ConstantInt::getSigned(i32, -1);
  llvm::Value *fractional = llvm::ConstantFP::getNaN(f64);
  auto split = [&](llvm::Value *value,
                   const Type *type) -> std::pair<llvm::Value *, llvm::Value *> {
    if (!type->isOptional()) {
      return {value, this->lowering.builder.getTrue()};
    }

    return {this->lowering.optionalLowerer().optionalValue(value, type),
            this->lowering.optionalLowerer().optionalHas(value, type)};
  };
  auto mark = [&](llvm::Value *has, unsigned bit) {
    present = this->lowering.builder.CreateOr(
        present, this->lowering.builder.CreateShl(this->lowering.builder.CreateZExt(has, i32),
                                                  this->lowering.builder.getInt32(bit)));
  };
  if (!args.empty()) {
    if (args[0]->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(args[0]);
    } else {
      Lowering::Operand locale = this->lowering.refCounts().emitOperand(
          args[0], args.size() < 2 || AssignLowerer::cannotAssign(args[1]));
      held.push_back(locale);
      auto [value, has] = split(locale.value, locale.type);
      this->lowering.builder.CreateStore(
          value, this->lowering.builder.CreateStructGEP(optionsTy, options, 0));
      mark(has, 31);
    }
  }
  if (args.size() > 1) {
    if (args[1]->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(args[1]);
    } else {
      Lowering::Operand object = this->lowering.refCounts().emitOperand(args[1], true);
      held.push_back(object);
      for (const Field &field : object.type->fields()) {
        llvm::Value *loaded = this->lowering.builder.CreateLoad(
            this->lowering.moduleBuilder().lower(field.type),
            this->lowering.objectLayouts().fieldAddress(object.value, object.type, field.name),
            field.name);
        auto [value, has] = split(loaded, field.type);
        const Type *type = field.type->nonOptional();
        if (field.name == "hour12") {
          hour12 = this->lowering.builder.CreateSelect(
              has, this->lowering.builder.CreateZExt(value, i32), hour12);
        } else if (field.name == "fractionalSecondDigits") {
          llvm::Value *digits =
              type->isFloat() ? value : this->lowering.builder.CreateSIToFP(value, f64);
          fractional = this->lowering.builder.CreateSelect(has, digits, fractional);
        } else {
          unsigned index =
              unsigned(llvm::find(kOptionNames, field.name) - std::begin(kOptionNames));
          if (!type->isString()) {
            value = this->lowering.exprLowerer().emitConversion(value, type, Type::string());
          }
          this->lowering.builder.CreateStore(
              value, this->lowering.builder.CreateConstGEP2_32(
                         valuesTy, this->lowering.builder.CreateStructGEP(optionsTy, options, 1), 0,
                         index));
          mark(has, index);
        }
      }
    }
  }
  this->lowering.builder.CreateStore(present,
                                     this->lowering.builder.CreateStructGEP(optionsTy, options, 2));
  this->lowering.builder.CreateStore(hour12,
                                     this->lowering.builder.CreateStructGEP(optionsTy, options, 3));
  this->lowering.builder.CreateStore(fractional,
                                     this->lowering.builder.CreateStructGEP(optionsTy, options, 4));

  return options;
}

llvm::Value *DateLowerer::parseResult(llvm::Value *time, llvm::Value *text, CallExpr *call) {
  static const std::vector<MakerFields> orders = {{nullptr, {"code", "message", "text"}}};
  const Type *type = call->type;
  const Type *errorType = type->errType();
  llvm::BasicBlock *badBlock = this->lowering.functionFrame().newBlock("date.bad");
  llvm::BasicBlock *goodBlock = this->lowering.functionFrame().newBlock("date.good");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("date.parsed");
  this->lowering.builder.CreateCondBr(this->lowering.builder.CreateFCmpUNO(time, time), badBlock,
                                      goodBlock);
  this->lowering.functionFrame().switchTo(badBlock);
  llvm::StructType *partsType =
      llvm::StructType::get(this->lowering.ctx, {this->lowering.builder.getPtrTy(),
                                                 this->lowering.strTy, this->lowering.strTy});
  llvm::Value *parts = this->lowering.functionFrame().createStackSlot(partsType, "date.error");
  this->lowering.builder.CreateStore(
      this->lowering.stringLowerer().utf8Constant("ERR_INVALID_DATE"),
      this->lowering.builder.CreateStructGEP(partsType, parts, 0));
  this->lowering.builder.CreateStore(this->lowering.stringLowerer().stringLiteral(u"Invalid Date"),
                                     this->lowering.builder.CreateStructGEP(partsType, parts, 1));
  this->lowering.refCounts().retainValue(text, Type::string());
  this->lowering.builder.CreateStore(text,
                                     this->lowering.builder.CreateStructGEP(partsType, parts, 2));
  llvm::Value *error = this->lowering.builder.CreateCall(
      this->lowering.objectMaker().maker(errorType, orders), {parts});
  llvm::Value *bad =
      this->lowering.resultLowerer().makeResult(type, this->lowering.builder.getFalse(), nullptr,
                                                error, this->lowering.errSites().site(call->loc()));
  llvm::BasicBlock *badEnd = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(goodBlock);
  llvm::Value *good = this->lowering.resultLowerer().makeResult(
      type, this->lowering.builder.getTrue(), this->wholeTime(time), nullptr);
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = this->lowering.builder.CreatePHI(good->getType(), 2, "date.result");
  phi->addIncoming(bad, badEnd);
  phi->addIncoming(good, goodBlock);

  return phi;
}

llvm::Value *DateLowerer::wholeTime(llvm::Value *time) {
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();

  return this->lowering.builder.CreateIntrinsic(llvm::Intrinsic::fptosi_sat, {i64, time->getType()},
                                                {time});
}

llvm::Value *DateLowerer::emitDateCall(CallExpr *call) {
  llvm::Type *voidTy = this->lowering.builder.getVoidTy(),
             *i32 = this->lowering.builder.getInt32Ty(), *i64 = this->lowering.builder.getInt64Ty();
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto constant = [&](size_t i) {
    return this->lowering.builder.getInt32(uint32_t(cast<IntLitExpr>(call->args[i])->value));
  };
  auto timeOf = [&](llvm::Value *date) {
    return this->lowering.builder.CreateLoad(i64, this->dateTimeAddress(date), "time");
  };
  auto now = [&] {
    return this->lowering.builder.CreateCall(
        this->lowering.processLowerer().processFunction("tz_date_now", i64, {}));
  };
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
  auto number = [&](Expr *arg) {
    llvm::Value *value = this->lowering.exprLowerer().emitExpr(arg);

    return arg->type->isFloat() ? this->wholeTime(value) : value;
  };

  switch (call->builtin()) {
    case Builtin::DateNew: {
      return this->newDate(call->args.empty() ? now() : number(call->args[0]));
    }
    case Builtin::DateCopy: {
      Lowering::Operand date = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *time = timeOf(date.value);
      this->lowering.refCounts().dropOperand(date);
      return this->newDate(time);
    }
    case Builtin::DateFromString:
    case Builtin::DateParse: {
      Lowering::Operand text = this->lowering.refCounts().emitOperand(call->args[0], true);
      bool parse = call->builtin() == Builtin::DateParse;
      llvm::Value *time = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction(
              parse ? "tz_date_parse" : "tz_date_from_text", parse ? f64 : i64, {ptr, i64}),
          {this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value)});
      llvm::Value *result = parse ? this->parseResult(time, text.value, call) : this->newDate(time);
      this->lowering.refCounts().dropOperand(text);
      return result;
    }
    case Builtin::DateFromFields:
    case Builtin::DateUTC: {
      std::vector<llvm::Value *> values;
      for (Expr *arg : call->args) {
        values.push_back(number(arg));
      }
      for (size_t i = values.size(); i < 7; ++i) {
        values.push_back(this->lowering.builder.getInt64(i == 2 ? 1 : 0));
      }
      values.push_back(this->lowering.builder.getInt32(call->builtin() == Builtin::DateFromFields));
      llvm::Value *time = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_date_from_fields", i64,
                                                          {i64, i64, i64, i64, i64, i64, i64, i32}),
          values);
      return call->builtin() == Builtin::DateUTC ? time : this->newDate(time);
    }
    case Builtin::DateText:
      return stringOf("tz_date_local_string",
                      {now(), this->lowering.builder.getInt32(TZ_DATE_TEXT_FULL)}, {i64, i32});
    case Builtin::DateGetTime:
    case Builtin::DateGetUTC:
    case Builtin::DateGetLocal: {
      Lowering::Operand date = receiver(call->args.size());
      llvm::Value *time = timeOf(date.value);
      this->lowering.refCounts().dropOperand(date);
      if (call->builtin() == Builtin::DateGetTime) {
        return time;
      }
      bool utc = call->builtin() == Builtin::DateGetUTC;
      return this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction(
              utc ? "tz_date_utc_field" : "tz_date_local_field", i64, {i64, i32}),
          {time, constant(0)});
    }
    case Builtin::DateSetTime:
    case Builtin::DateSetUTC:
    case Builtin::DateSetLocal: {
      Lowering::Operand date = receiver(0);
      bool setTime = call->builtin() == Builtin::DateSetTime;
      std::vector<llvm::Value *> values;
      for (size_t i = setTime ? 0 : 1; i < call->args.size(); ++i) {
        values.push_back(number(call->args[i]));
      }
      llvm::Value *address = this->dateTimeAddress(date.value);
      llvm::Value *time;
      if (setTime) {
        time = values[0];
      } else {
        llvm::Value *count = this->lowering.builder.getInt32(uint32_t(values.size()));
        while (values.size() < 4) {
          values.push_back(this->lowering.builder.getInt64(0));
        }
        time = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction(
                "tz_date_set", i64, {i64, i32, i32, i32, i64, i64, i64, i64}),
            {this->lowering.builder.CreateLoad(i64, address, "time"), constant(0),
             this->lowering.builder.getInt32(call->builtin() == Builtin::DateSetLocal), count,
             values[0], values[1], values[2], values[3]});
      }
      this->lowering.builder.CreateStore(time, address);
      this->lowering.refCounts().dropOperand(date);
      return time;
    }
    case Builtin::DateToISOString:
    case Builtin::DateToUTCString:
    case Builtin::DateToLocalString:
    case Builtin::DateToJSON: {
      Lowering::Operand date = receiver(0);
      if (call->builtin() == Builtin::DateToJSON && !call->args.empty()) {
        this->lowering.stmtLowerer().emitDiscarded(call->args[0]);
      }
      llvm::Value *time = timeOf(date.value);
      this->lowering.refCounts().dropOperand(date);
      if (call->builtin() == Builtin::DateToUTCString) {
        return stringOf("tz_date_utc_string", {time}, {i64});
      }
      if (call->builtin() == Builtin::DateToLocalString) {
        return stringOf("tz_date_local_string", {time, constant(0)}, {i64, i32});
      }
      return stringOf("tz_date_iso", {time}, {i64});
    }
    case Builtin::DateToLocale: {
      Lowering::Operand date = receiver(1);
      std::vector<Lowering::Operand> held;
      llvm::ArrayRef<Expr *> args = llvm::ArrayRef<Expr *>(call->args).drop_front();
      llvm::Value *options = this->emitDateOptions(args, held);
      llvm::Value *time = timeOf(date.value);
      llvm::Value *text = stringOf("tz_date_locale", {time, constant(0), options}, {i64, i32, ptr});
      for (const Lowering::Operand &operand : held) {
        this->lowering.refCounts().dropOperand(operand);
      }
      this->lowering.refCounts().dropOperand(date);
      return text;
    }
    case Builtin::DtfNew: {
      std::vector<Lowering::Operand> held;
      llvm::Value *options = this->emitDateOptions(call->args, held);
      llvm::Value *format = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_dtf_new", ptr, {ptr}), {options});
      for (const Lowering::Operand &operand : held) {
        this->lowering.refCounts().dropOperand(operand);
      }
      return format;
    }
    case Builtin::DtfFormat:
    case Builtin::DtfFormatToParts: {
      Lowering::Operand format = receiver(0);
      llvm::Value *time = nullptr;
      if (call->args.empty()) {
        time = now();
      } else if (call->args[0]->type->isDate()) {
        Lowering::Operand date = this->lowering.refCounts().emitOperand(call->args[0], true);
        time = timeOf(date.value);
        this->lowering.refCounts().dropOperand(date);
      } else {
        time = number(call->args[0]);
      }
      llvm::Value *result;
      if (call->builtin() == Builtin::DtfFormat) {
        result = stringOf("tz_dtf_format", {format.value, time}, {ptr, i64});
      } else {
        const Type *part = call->type->inner();
        const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(part);
        const llvm::StructLayout *offsets =
            this->lowering.module_->getDataLayout().getStructLayout(layout.type);
        auto offsetOf = [&](llvm::StringRef name) {
          return this->lowering.builder.getInt32(uint32_t(
              offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(part, name)])));
        };
        result = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_dtf_format_to_parts", ptr,
                                                            {ptr, i64, ptr, ptr, i64, i32, i32}),
            {format.value, time, this->lowering.typeDescriptors().typeDescriptor(part),
             this->lowering.objectLayouts().layoutDescriptor(part, {0, 1}),
             this->lowering.builder.getInt64(
                 this->lowering.module_->getDataLayout().getTypeAllocSize(layout.type)),
             offsetOf("type"), offsetOf("value")},
            "parts");
      }
      this->lowering.refCounts().dropOperand(format);
      return result;
    }
    case Builtin::DtfResolvedOptions: {
      Lowering::Operand format = receiver(0);
      const Type *type = call->type;
      std::vector<unsigned> keys(type->fields().size());
      std::iota(keys.begin(), keys.end(), 0u);
      llvm::Value *object = this->lowering.objectLayouts().allocObject(type, keys);
      for (const Field &field : type->fields()) {
        llvm::Value *address =
            this->lowering.objectLayouts().fieldAddress(object, type, field.name);
        llvm::Value *value;
        if (field.name == "hour12") {
          llvm::Value *v = this->lowering.builder.CreateCall(
              this->lowering.processLowerer().processFunction("tz_dtf_hour12", i32, {ptr}),
              {format.value});
          value = this->lowering.optionalLowerer().makeOptional(
              this->lowering.builder.CreateICmpEQ(v, this->lowering.builder.getInt32(1)),
              this->lowering.builder.CreateICmpSGE(v, this->lowering.builder.getInt32(0)),
              field.type);
        } else if (field.name == "fractionalSecondDigits") {
          llvm::Value *v = this->lowering.builder.CreateCall(
              this->lowering.processLowerer().processFunction("tz_dtf_fractional", i32, {ptr}),
              {format.value});
          value = this->lowering.optionalLowerer().makeOptional(
              this->lowering.builder.CreateSExt(v, this->lowering.builder.getInt64Ty()),
              this->lowering.builder.CreateICmpSGT(v, this->lowering.builder.getInt32(0)),
              field.type);
        } else {
          auto at = llvm::find(kOptionNames, field.name);
          int which = at == std::end(kOptionNames) ? -1 : int(at - std::begin(kOptionNames));
          llvm::AllocaInst *out =
              this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "option.out");
          llvm::Value *has = this->lowering.builder.CreateCall(
              this->lowering.processLowerer().processFunction("tz_dtf_option", i32,
                                                              {ptr, ptr, i32}),
              {out, format.value, this->lowering.builder.getInt32(uint32_t(which))});
          llvm::Value *text = this->lowering.builder.CreateLoad(this->lowering.strTy, out);
          value = field.type->isOptional() ? this->lowering.optionalLowerer().makeOptional(
                                                 text,
                                                 this->lowering.builder.CreateICmpNE(
                                                     has, this->lowering.builder.getInt32(0)),
                                                 field.type)
                                           : text;
        }
        this->lowering.builder.CreateStore(value, address);
      }
      this->lowering.refCounts().dropOperand(format);
      return object;
    }
    default:
      llvm_unreachable("not a built-in of Date");
  }
}

}
