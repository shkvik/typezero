#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
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

namespace {

constexpr size_t kMostDateArgs = 7;

}

DateChecker::DateChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeRelations = &all.typeRelations;
  this->binaryChecker = &all.binaryChecker;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
  this->dateFormatChecker = &all.dateFormatChecker;
}

std::optional<std::u16string> DateChecker::literalText(const Expr *expr) const {
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

bool DateChecker::isLeap(int64_t year) const {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DateChecker::daysInMonth(int64_t year, int64_t month) const {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  return month == 1 && this->isLeap(year) ? 29 : days[month];
}

std::pair<int64_t, int64_t> DateChecker::carriedDay(int64_t year, int64_t month,
                                                    int64_t day) const {
  int64_t currentYear = year;
  int64_t currentMonth = month;
  int64_t currentDay = day;
  while (currentDay > this->daysInMonth(currentYear, currentMonth)) {
    currentDay -= this->daysInMonth(currentYear, currentMonth);
    if (++currentMonth == 12) {
      currentMonth = 0;
      ++currentYear;
    }
  }

  return {currentMonth, currentDay};
}

std::string DateChecker::missingDay(std::optional<int64_t> year, int64_t month, int64_t day) const {
  std::pair<int64_t, int64_t> carried = year.has_value() ? this->carriedDay(*year, month, day)
                                                         : std::pair<int64_t, int64_t>(month, day);

  return ErrorTextDateDay::missing(year, month, day, carried.first, carried.second);
}

DateChecker::ResultLiteralDate DateChecker::checkLiteralDate(const std::u16string &text) const {
  std::vector<uint16_t> units(text.begin(), text.end());
  TzDateFields fields;
  auto shown = ToUtf8::toUtf8(text);
  if (!tz_date_parse_fields(units.data(), units.size(), &fields)) {
    auto message = ErrorTextDateLiteral::unreadable(shown);

    return err(DateCheckerLiteralDateError{
        .code = DateCheckerLiteralDateErrorCode::UNREADABLE,
        .message = message,
    });
  }
  if (fields.date_only) {
    auto message = ErrorTextDateLiteral::dateOnly(shown);

    return err(DateCheckerLiteralDateError{
        .code = DateCheckerLiteralDateErrorCode::DATE_ONLY,
        .message = message,
    });
  }
  int64_t month = int64_t(fields.month);
  int64_t dayOfMonth = int64_t(fields.day);
  if (dayOfMonth > this->daysInMonth(int64_t(fields.year), month)) {
    auto missing = this->missingDay(int64_t(fields.year), month, dayOfMonth);
    auto message = ErrorTextDateLiteral::noDay(shown, missing);

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
    auto message = ErrorTextDateLiteral::outside(shown);

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
  auto text = this->literalText(arg);
  if (!text) {
    return ok();
  }
  auto date = this->checkLiteralDate(*text);
  if (date.isErr()) {
    return err(date.error());
  }

  return ok();
}

void DateChecker::walkDateFields(std::vector<Expr *> &args, size_t first, llvm::StringRef setter) {
  DateFieldValues fields{.first = first, .setter = setter, .constant = {}};
  for (size_t index = 0; index < args.size(); ++index) {
    args[index] = this->timeArg(args[index]);
    if (args[index]->type->isInteger()) {
      fields.constant[first + index] = this->constantFolder->intConstant(args[index]);
    }
  }
  this->walkYearField(args, fields);
  auto month = this->validMonth(args, fields);
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
    auto message = ErrorTextDateFields::shortYear(*year);
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
    auto message = ErrorTextDateFields::dayBelow(*day);
    this->reportField(args, fields, 2, message);
    return;
  }
  if (month.has_value()) {
    this->walkDayInMonth(args, fields, *day, *month);
    return;
  }
  if (*day > 31) {
    auto message = ErrorTextDateFields::dayAbove(*day);
    this->reportField(args, fields, 2, message);
  }
}

void DateChecker::walkDayInMonth(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                                 int64_t day, int64_t month) {
  std::optional<int64_t> year = fields.constant[0];
  bool isShortYear = fields.setter.empty() && year.has_value() && *year >= 0 && *year <= 99;
  int64_t fullYear = isShortYear ? 1900 + *year : year.value_or(0);
  std::optional<int64_t> known = year.has_value() ? std::optional<int64_t>(fullYear) : std::nullopt;
  int last = this->lastDayOf(known, month);
  if (day <= last) {
    return;
  }
  auto missing = this->missingDay(known, month, day);
  auto message = this->dayOverflowText(fields.setter, missing, month, known, last);
  this->reportField(args, fields, 2, message);
}

int DateChecker::lastDayOf(std::optional<int64_t> year, int64_t month) const {
  if (year.has_value()) {
    return this->daysInMonth(*year, month);
  }
  if (month == 1) {
    return 29;
  }

  return this->daysInMonth(2001, month);
}

std::string DateChecker::dayOverflowText(llvm::StringRef setter, const std::string &missing,
                                         int64_t month, std::optional<int64_t> year,
                                         int last) const {
  if (setter.empty()) {
    auto nextYear = this->nextMonthYear(year, month);
    int64_t next = month == 11 ? 0 : month + 1;
    return ErrorTextDateFields::lastDay(missing, month, nextYear, next);
  }
  if (month == 1) {
    return ErrorTextDateFields::february(missing, setter, year);
  }

  return ErrorTextDateFields::monthDays(missing, month, last);
}

std::optional<int64_t> DateChecker::nextMonthYear(std::optional<int64_t> year,
                                                  int64_t month) const {
  if (!year.has_value()) {
    return std::nullopt;
  }

  return month == 11 ? *year + 1 : *year;
}

void DateChecker::walkTimeFields(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields) {
  static const struct {
    const char *what;
    int64_t max;
  } limits[] = {{"hours", 23}, {"minutes", 59}, {"seconds", 59}, {"milliseconds", 999}};
  size_t end = fields.first + args.size();
  for (size_t index = std::max<size_t>(fields.first, 3); index < end; ++index) {
    std::optional<int64_t> value = fields.constant[index];
    if (value.has_value() && (*value < 0 || *value > limits[index - 3].max)) {
      auto message =
          ErrorTextDateFields::range(*value, limits[index - 3].what, limits[index - 3].max);
      this->reportField(args, fields, index, message);
    }
  }
}

Expr *DateChecker::timeArg(Expr *arg) {
  if (arg->type == nullptr) {
    arg = this->exprChecker->checkExpr(arg, Type::intTy());
  }
  arg = this->genericChecker->apparent(arg);
  if (arg->type->isInteger()) {
    return arg->type->isInt() ? arg : this->typeRelations->convert(arg, Type::intTy());
  }

  return this->typeRelations->coerce(arg, Type::intTy());
}

Expr *DateChecker::checkDateNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto dateName = GlobalNameTable::getName(GlobalName::DATE);
  auto *callee = this->state.getAst().make<NameExpr>(loc, dateName);
  if (args.size() > kMostDateArgs) {
    auto message = ErrorTextDateNew::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    for (Expr *&arg : args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return this->failedDate(loc);
  }
  if (args.empty()) {
    return this->processChecker->builtinCall(loc, callee, Builtin::DateNew, {}, Type::date());
  }
  if (args.size() == 1) {
    return this->checkDateFromValue(loc, callee, args[0]);
  }
  this->walkDateFields(args);

  return this->processChecker->builtinCall(loc, callee, Builtin::DateFromFields, args,
                                           Type::date());
}

Expr *DateChecker::checkDateFromValue(llvm::SMLoc loc, Expr *callee, Expr *given) {
  auto literal = this->checkDateLiteral(given);
  if (literal.isErr()) {
    this->state.getDiagnostics().error(given->loc(), literal.error().message);
  }
  auto *checked = this->exprChecker->checkExpr(given);
  auto *arg = this->genericChecker->apparent(checked);
  const Type *type = arg->type;
  if (type->isError()) {
    return this->failedDate(loc);
  }
  if (type->isDate()) {
    return this->processChecker->builtinCall(loc, callee, Builtin::DateCopy, {arg}, Type::date());
  }
  if (type->isString()) {
    return this->checkDateFromString(loc, callee, given, arg);
  }
  if (type->isNumeric()) {
    auto *time = this->timeArg(arg);
    return this->processChecker->builtinCall(loc, callee, Builtin::DateNew, {time}, Type::date());
  }
  auto message = ErrorTextDateValue::takes(type);
  this->state.getDiagnostics().error(arg->loc(), message);

  return this->failedDate(loc);
}

Expr *DateChecker::checkDateFromString(llvm::SMLoc loc, Expr *callee, const Expr *given,
                                       Expr *arg) {
  if (!this->literalText(given)) {
    auto message = ErrorTextDateNew::unknown();
    this->state.getDiagnostics().error(arg->loc(), message);
    return this->failedDate(loc);
  }

  return this->processChecker->builtinCall(loc, callee, Builtin::DateFromString, {arg},
                                           Type::date());
}

Expr *DateChecker::failedDate(llvm::SMLoc loc) {
  auto *error = this->state.getAst().make<UndefinedExpr>(loc);
  error->type = Type::error();

  return error;
}

Expr *DateChecker::checkDateFunction(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (!call->args.empty()) {
    auto message = ErrorTextDateNew::call();
    this->state.getDiagnostics().error(name->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
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
    auto arity = this->argumentChecker->checkArity(call, 1, 7);
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
    auto arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    auto literal = this->checkDateLiteral(call->args[0]);
    if (literal.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), literal.error().message);
    }
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
    call->target = BuiltinCall{.builtin = Builtin::DateParse};
    call->type = Type::result(Type::intTy(), this->prelude->builtinType(PreludeType::DATE_ERROR));
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
  const auto *method = DateMethodTable::find(member->name);
  if (method == nullptr) {
    auto message = this->unknownMethodMessage(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, method->min, method->max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = method->builtin};

  return this->checkDateMethodForm(call, member, *method);
}

std::string DateChecker::unknownMethodMessage(llvm::StringRef name) const {
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
  auto *literal = this->state.getAst().make<IntLitExpr>(member->loc(), uint64_t(value));
  literal->type = Type::intTy();

  return literal;
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
  if (!this->dateFormatChecker->prepareLocaleArgs(call->args, method.field)) {
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

Expr *DateChecker::checkDateOperands(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (!leftType->isDate() && !rightType->isDate()) {
    return nullptr;
  }
  binary->type = Type::error();
  bool isCompare = binary->op == BinaryOp::Lt || binary->op == BinaryOp::Le ||
                   binary->op == BinaryOp::Gt || binary->op == BinaryOp::Ge;
  if (leftType->isDate() && rightType->isDate() && (isCompare || binary->op == BinaryOp::Sub)) {
    binary->lhs = this->dateTime(binary->lhs);
    binary->rhs = this->dateTime(binary->rhs);
    binary->type = isCompare ? Type::boolTy() : Type::intTy();
    return binary;
  }
  auto message = this->dateOperatorText(binary, isCompare);
  this->state.getDiagnostics().error(binary->loc(), message);

  return binary;
}

std::string DateChecker::dateOperatorText(const BinaryExpr *binary, bool isCompare) const {
  const char *op = Operators::opSpelling(binary->op);
  if (binary->op == BinaryOp::Add) {
    return ErrorTextDateOperator::plus();
  }
  if (binary->op == BinaryOp::Sub) {
    return ErrorTextDateOperator::minus();
  }
  if (isCompare) {
    return ErrorTextDateOperator::compare(op);
  }

  return this->binaryChecker->operandTypesText(op, binary->lhs->type, binary->rhs->type);
}

}
