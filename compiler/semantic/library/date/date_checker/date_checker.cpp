#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/program/library/date/date_method_table/date_method_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

DateChecker::DateChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeRelations = &all.typeRelations;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
  this->dateFormatChecker = &all.dateFormatChecker;
}

std::optional<std::u16string> DateChecker::literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

bool DateChecker::isLeap(int64_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DateChecker::daysInMonth(int64_t year, int64_t month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  return month == 1 && DateChecker::isLeap(year) ? 29 : days[month];
}

std::pair<int64_t, int64_t> DateChecker::carriedDay(int64_t year, int64_t month, int64_t day) {
  int64_t y = year;
  int64_t m = month;
  int64_t d = day;
  while (d > DateChecker::daysInMonth(y, m)) {
    d -= DateChecker::daysInMonth(y, m);
    if (++m == 12) {
      m = 0;
      ++y;
    }
  }

  return {m, d};
}

std::string DateChecker::missingDay(std::optional<int64_t> year, int64_t month, int64_t day) {
  std::pair<int64_t, int64_t> carried =
      year ? DateChecker::carriedDay(*year, month, day) : std::pair<int64_t, int64_t>(month, day);

  return ErrorTextDateDay::missing(year, month, day, carried.first, carried.second);
}

DateChecker::ResultLiteralDate DateChecker::literalDate(const std::u16string &text) {
  std::vector<uint16_t> units(text.begin(), text.end());
  TzDateFields fields;
  std::string shown = ToUtf8::toUtf8(text);
  if (!tz_date_parse_fields(units.data(), units.size(), &fields)) {
    std::string message = ErrorTextDateLiteral::unreadable(shown);

    return err(DateCheckerLiteralDateError{
        .code = DateCheckerLiteralDateErrorCode::UNREADABLE,
        .message = message,
    });
  }
  if (fields.date_only) {
    std::string message = ErrorTextDateLiteral::dateOnly(shown);

    return err(DateCheckerLiteralDateError{
        .code = DateCheckerLiteralDateErrorCode::DATE_ONLY,
        .message = message,
    });
  }
  int64_t month = int64_t(fields.month);
  int64_t dayOfMonth = int64_t(fields.day);
  if (dayOfMonth > DateChecker::daysInMonth(int64_t(fields.year), month)) {
    std::string missing = DateChecker::missingDay(int64_t(fields.year), month, dayOfMonth);
    std::string message = ErrorTextDateLiteral::noDay(shown, missing);

    return err(DateCheckerLiteralDateError{
        .code = DateCheckerLiteralDateErrorCode::NO_SUCH_DAY,
        .message = message,
    });
  }
  double day = tz_date_make_day(fields.year, fields.month, fields.day);
  double time = tz_date_make_time(fields.hour, fields.minute, fields.second, fields.ms);
  double value = tz_date_make_date(day, time);
  double limit = TZ_DATE_MAX_TIME;
  if (std::isnan(fields.offset)) {
    limit = TZ_DATE_MAX_LOCAL_TIME;
  } else {
    value -= fields.offset * 1000;
  }
  if (!(value >= -limit && value <= limit)) {
    std::string message = ErrorTextDateLiteral::outside(shown);

    return err(DateCheckerLiteralDateError{
        .code = DateCheckerLiteralDateErrorCode::OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(value);
}

Expr *DateChecker::dateTime(Expr *date) {
  auto *callee = this->state.getAst().make<MemberExpr>(date->loc(), date, "getTime");

  return this->processChecker->builtinCall(date->loc(), callee, Builtin::DateGetTime, {},
                                           Type::intTy());
}

Expr *DateChecker::dateToString(Expr *date) {
  auto *callee = this->state.getAst().make<MemberExpr>(date->loc(), date, "toString");
  auto *mode = this->state.getAst().make<IntLitExpr>(date->loc(), TZ_DATE_TEXT_FULL);
  mode->type = Type::intTy();

  return this->processChecker->builtinCall(date->loc(), callee, Builtin::DateToLocalString, {mode},
                                           Type::string());
}

DateChecker::ResultDateLiteralChecked DateChecker::checkDateLiteral(const Expr *arg) {
  std::optional<std::u16string> text = DateChecker::literalText(arg);
  if (!text) {
    return ok();
  }
  ResultLiteralDate date = DateChecker::literalDate(*text);
  if (date.isErr()) {
    return err(date.error());
  }

  return ok();
}

void DateChecker::walkDateFields(std::vector<Expr *> &args, size_t first, llvm::StringRef setter) {
  DateFieldValues fields{.first = first, .setter = setter, .constant = {}};
  for (size_t i = 0; i < args.size(); ++i) {
    args[i] = this->timeArg(args[i]);
    if (args[i]->type->isInteger()) {
      fields.constant[first + i] = this->constantFolder->intConstant(args[i]);
    }
  }
  this->walkYearField(args, fields);
  std::optional<int64_t> month = this->validMonth(args, fields);
  this->walkDayField(args, fields, month);
  this->walkTimeFields(args, fields);
}

void DateChecker::reportField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                              size_t field, const std::string &message) {
  this->state.getDiagnostics().error(args[field - fields.first]->loc(), message);
}

void DateChecker::walkYearField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields) {
  std::optional<int64_t> year = fields.constant[0];
  bool isShortYear = fields.setter.empty() && year.has_value() && *year >= 0 && *year <= 99;
  if (isShortYear) {
    std::string message = ErrorTextDateFields::shortYear(*year);
    this->reportField(args, fields, 0, message);
  }
}

std::optional<int64_t> DateChecker::validMonth(llvm::ArrayRef<Expr *> args,
                                               const DateFieldValues &fields) {
  std::optional<int64_t> month = fields.constant[1];
  if (!month.has_value() || (*month >= 0 && *month <= 11)) {
    return month;
  }
  std::string message =
      *month == 12 ? ErrorTextDateFields::month12() : ErrorTextDateFields::month(*month);
  this->reportField(args, fields, 1, message);

  return std::nullopt;
}

void DateChecker::walkDayField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                               std::optional<int64_t> month) {
  std::optional<int64_t> day = fields.constant[2];
  if (!day.has_value()) {
    return;
  }
  if (*day < 0) {
    std::string message = ErrorTextDateFields::dayBelow(*day);
    this->reportField(args, fields, 2, message);
    return;
  }
  if (month.has_value()) {
    this->walkDayInMonth(args, fields, *day, *month);
    return;
  }
  if (*day > 31) {
    std::string message = ErrorTextDateFields::dayAbove(*day);
    this->reportField(args, fields, 2, message);
  }
}

void DateChecker::walkDayInMonth(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                                 int64_t day, int64_t month) {
  llvm::StringRef setter = fields.setter;
  std::optional<int64_t> year = fields.constant[0];
  bool isShortYear = setter.empty() && year.has_value() && *year >= 0 && *year <= 99;
  int64_t fullYear = isShortYear ? 1900 + *year : year.value_or(0);
  int last = year.has_value() ? DateChecker::daysInMonth(fullYear, month)
             : month == 1     ? 29
                              : DateChecker::daysInMonth(2001, month);
  if (day <= last) {
    return;
  }
  std::optional<int64_t> known = year.has_value() ? std::optional<int64_t>(fullYear) : std::nullopt;
  std::string missing = DateChecker::missingDay(known, month, day);
  std::optional<int64_t> nextYear =
      year.has_value() ? std::optional<int64_t>(month == 11 ? fullYear + 1 : fullYear)
                       : std::nullopt;
  int64_t next = month == 11 ? 0 : month + 1;
  std::string message = setter.empty()
                            ? ErrorTextDateFields::lastDay(missing, month, nextYear, next)
                        : month == 1 ? ErrorTextDateFields::february(missing, setter, known)
                                     : ErrorTextDateFields::monthDays(missing, month, last);
  this->reportField(args, fields, 2, message);
}

void DateChecker::walkTimeFields(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields) {
  static const struct {
    const char *what;
    int64_t max;
  } limits[] = {{"hours", 23}, {"minutes", 59}, {"seconds", 59}, {"milliseconds", 999}};
  size_t end = fields.first + args.size();
  for (size_t i = std::max<size_t>(fields.first, 3); i < end; ++i) {
    std::optional<int64_t> value = fields.constant[i];
    if (value.has_value() && (*value < 0 || *value > limits[i - 3].max)) {
      std::string message =
          ErrorTextDateFields::range(*value, limits[i - 3].what, limits[i - 3].max);
      this->reportField(args, fields, i, message);
    }
  }
}

Expr *DateChecker::timeArg(Expr *arg) {
  if (!arg->type) {
    arg = this->exprChecker->checkExpr(arg, Type::intTy());
  }
  arg = this->genericChecker->apparent(arg);
  if (arg->type->isInteger()) {
    return arg->type->isInt() ? arg : this->typeRelations->convert(arg, Type::intTy());
  }

  return this->typeRelations->coerce(arg, Type::intTy());
}

Expr *DateChecker::checkDateNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto *callee = this->state.getAst().make<NameExpr>(loc, "Date");
  auto failed = [&] {
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  if (args.size() > 7) {
    std::string message = ErrorTextDateNew::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    for (Expr *&arg : args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return failed();
  }
  if (args.empty()) {
    return this->processChecker->builtinCall(loc, callee, Builtin::DateNew, {}, Type::date());
  }
  if (args.size() == 1) {
    ResultDateLiteralChecked literal = this->checkDateLiteral(args[0]);
    if (literal.isErr()) {
      this->state.getDiagnostics().error(args[0]->loc(), literal.error().message);
    }
    Expr *arg = this->genericChecker->apparent(this->exprChecker->checkExpr(args[0]));
    const Type *type = arg->type;
    if (type->isError()) {
      return failed();
    }
    if (type->isDate()) {
      return this->processChecker->builtinCall(loc, callee, Builtin::DateCopy, {arg}, Type::date());
    }
    if (type->isString()) {
      if (!DateChecker::literalText(args[0])) {
        std::string message = ErrorTextDateNew::unknown();
        this->state.getDiagnostics().error(arg->loc(), message);
        return failed();
      }
      return this->processChecker->builtinCall(loc, callee, Builtin::DateFromString, {arg},
                                               Type::date());
    }
    if (type->isNumeric()) {
      return this->processChecker->builtinCall(loc, callee, Builtin::DateNew, {this->timeArg(arg)},
                                               Type::date());
    }
    std::string message = ErrorTextDateValue::takes(type);
    this->state.getDiagnostics().error(arg->loc(), message);
    return failed();
  }
  this->walkDateFields(args);

  return this->processChecker->builtinCall(loc, callee, Builtin::DateFromFields, args,
                                           Type::date());
}

Expr *DateChecker::checkDateFunction(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (!call->args.empty()) {
    std::string message = ErrorTextDateNew::call();
    this->state.getDiagnostics().error(name->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::DateText};
  call->type = Type::string();

  return call;
}

Expr *DateChecker::checkDateStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  llvm::StringRef name = member->name;
  if (name == "UTC") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 7);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    this->walkDateFields(call->args);
    call->target = BuiltinCall{.builtin = Builtin::DateUTC};
    call->type = Type::intTy();
    return call;
  }
  if (name == "parse") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    ResultDateLiteralChecked literal = this->checkDateLiteral(call->args[0]);
    if (literal.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), literal.error().message);
    }
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
    call->target = BuiltinCall{.builtin = Builtin::DateParse};
    call->type = Type::result(Type::intTy(), this->prelude->builtinType("DateError"));
    return call;
  }
  this->checkDateStaticMember(member);
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *DateChecker::checkDateStaticMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  std::string message = name == "now" || name == "UTC" || name == "parse"
                            ? ErrorTextDateStatic::onlyCalled(name)
                            : ErrorTextDateStatic::noMember(name);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *DateChecker::checkDateMethod(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  const DateMethodInfo *method = DateMethodTable::find(member->name);
  if (method == nullptr) {
    std::string message = DateChecker::unknownMethodMessage(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, method->min, method->max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = method->builtin};

  return this->checkDateMethodForm(call, member, *method);
}

std::string DateChecker::unknownMethodMessage(llvm::StringRef name) {
  if (name == "getYear") {
    return ErrorTextDateMethod::getYear();
  }
  if (name == "setYear") {
    return ErrorTextDateMethod::setYear();
  }
  if (name == "toGMTString") {
    return ErrorTextDateMethod::gmt();
  }

  return ErrorTextDateMethod::noMember(name);
}

Expr *DateChecker::checkDateMethodForm(CallExpr *call, MemberExpr *member,
                                       const DateMethodInfo &method) {
  switch (method.form) {
    case DateMethodForm::GET_TIME:
      call->type = Type::intTy();
      return call;
    case DateMethodForm::GET_FIELD:
      call->args.push_back(this->fieldLiteral(member, method.field));
      call->type = Type::intTy();
      return call;
    case DateMethodForm::SET_TIME:
      call->args[0] = this->timeArg(this->exprChecker->checkExpr(call->args[0], Type::intTy()));
      call->type = Type::intTy();
      return call;
    case DateMethodForm::SET_FIELDS:
      return this->checkDateSetter(call, member, method);
    case DateMethodForm::TO_JSON:
      if (!call->args.empty()) {
        call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
      }
      call->type = Type::string();
      return call;
    case DateMethodForm::TO_TEXT:
      call->type = Type::string();
      return call;
    case DateMethodForm::TO_FIELD_TEXT:
      call->args.push_back(this->fieldLiteral(member, method.field));
      call->type = Type::string();
      return call;
    case DateMethodForm::TO_LOCALE:
      return this->checkToLocale(call, member, method);
  }

  return call;
}

Expr *DateChecker::fieldLiteral(const MemberExpr *member, int value) {
  auto *lit = this->state.getAst().make<IntLitExpr>(member->loc(), uint64_t(value));
  lit->type = Type::intTy();

  return lit;
}

Expr *DateChecker::checkDateSetter(CallExpr *call, MemberExpr *member,
                                   const DateMethodInfo &method) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg, Type::intTy());
  }
  size_t first = size_t(method.field >= TZ_DATE_HOURS ? method.field - 1 : method.field);
  this->walkDateFields(call->args, first, member->name);
  call->args.insert(call->args.begin(), this->fieldLiteral(member, method.field));
  call->type = Type::intTy();

  return call;
}

Expr *DateChecker::checkToLocale(CallExpr *call, MemberExpr *member, const DateMethodInfo &method) {
  if (!this->dateFormatChecker->checkLocaleArgs(call->args, method.field)) {
    return call;
  }
  call->args.insert(call->args.begin(), this->fieldLiteral(member, method.field));
  call->type = Type::string();

  return call;
}

Expr *DateChecker::checkDateMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  bool isMethod = DateMethodTable::find(name) != nullptr;
  std::string message = isMethod            ? ErrorTextDateMethod::onlyCalled(name)
                        : name == "getYear" ? ErrorTextDateMethod::getYearVal()
                                            : ErrorTextDateMethod::noMember(name);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *DateChecker::checkDateOperands(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (!l->isDate() && !r->isDate()) {
    return nullptr;
  }
  bin->type = Type::error();
  bool isCompare = bin->op == BinaryOp::Lt || bin->op == BinaryOp::Le || bin->op == BinaryOp::Gt ||
                   bin->op == BinaryOp::Ge;
  if (l->isDate() && r->isDate() && (isCompare || bin->op == BinaryOp::Sub)) {
    bin->lhs = this->dateTime(bin->lhs);
    bin->rhs = this->dateTime(bin->rhs);
    bin->type = isCompare ? Type::boolTy() : Type::intTy();
    return bin;
  }
  const char *op = Operators::opSpelling(bin->op);
  std::string message = bin->op == BinaryOp::Add   ? ErrorTextDateOperator::plus()
                        : bin->op == BinaryOp::Sub ? ErrorTextDateOperator::minus()
                        : isCompare                ? ErrorTextDateOperator::compare(op)
                                                   : BinaryChecker::operandTypesText(op, l, r);
  this->state.getDiagnostics().error(bin->loc(), message);

  return bin;
}

}
