#include "compiler/semantic/library/date/date_checker/date_checker.h"
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
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

namespace {

struct DateMethod {
  llvm::StringLiteral name;
  Builtin builtin;
  int field;
  unsigned min, max;
};

const DateMethod kMethods[] = {
    {"getTime", Builtin::DateGetTime, 0, 0, 0},
    {"valueOf", Builtin::DateGetTime, 0, 0, 0},
    {"getFullYear", Builtin::DateGetLocal, TZ_DATE_YEAR, 0, 0},
    {"getMonth", Builtin::DateGetLocal, TZ_DATE_MONTH, 0, 0},
    {"getDate", Builtin::DateGetLocal, TZ_DATE_DATE, 0, 0},
    {"getDay", Builtin::DateGetLocal, TZ_DATE_DAY, 0, 0},
    {"getHours", Builtin::DateGetLocal, TZ_DATE_HOURS, 0, 0},
    {"getMinutes", Builtin::DateGetLocal, TZ_DATE_MINUTES, 0, 0},
    {"getSeconds", Builtin::DateGetLocal, TZ_DATE_SECONDS, 0, 0},
    {"getMilliseconds", Builtin::DateGetLocal, TZ_DATE_MS, 0, 0},
    {"getTimezoneOffset", Builtin::DateGetLocal, TZ_DATE_OFFSET, 0, 0},
    {"getUTCFullYear", Builtin::DateGetUTC, TZ_DATE_YEAR, 0, 0},
    {"getUTCMonth", Builtin::DateGetUTC, TZ_DATE_MONTH, 0, 0},
    {"getUTCDate", Builtin::DateGetUTC, TZ_DATE_DATE, 0, 0},
    {"getUTCDay", Builtin::DateGetUTC, TZ_DATE_DAY, 0, 0},
    {"getUTCHours", Builtin::DateGetUTC, TZ_DATE_HOURS, 0, 0},
    {"getUTCMinutes", Builtin::DateGetUTC, TZ_DATE_MINUTES, 0, 0},
    {"getUTCSeconds", Builtin::DateGetUTC, TZ_DATE_SECONDS, 0, 0},
    {"getUTCMilliseconds", Builtin::DateGetUTC, TZ_DATE_MS, 0, 0},
    {"setTime", Builtin::DateSetTime, 0, 1, 1},
    {"setMilliseconds", Builtin::DateSetLocal, TZ_DATE_MS, 1, 1},
    {"setSeconds", Builtin::DateSetLocal, TZ_DATE_SECONDS, 1, 2},
    {"setMinutes", Builtin::DateSetLocal, TZ_DATE_MINUTES, 1, 3},
    {"setHours", Builtin::DateSetLocal, TZ_DATE_HOURS, 1, 4},
    {"setDate", Builtin::DateSetLocal, TZ_DATE_DATE, 1, 1},
    {"setMonth", Builtin::DateSetLocal, TZ_DATE_MONTH, 1, 2},
    {"setFullYear", Builtin::DateSetLocal, TZ_DATE_YEAR, 1, 3},
    {"setUTCMilliseconds", Builtin::DateSetUTC, TZ_DATE_MS, 1, 1},
    {"setUTCSeconds", Builtin::DateSetUTC, TZ_DATE_SECONDS, 1, 2},
    {"setUTCMinutes", Builtin::DateSetUTC, TZ_DATE_MINUTES, 1, 3},
    {"setUTCHours", Builtin::DateSetUTC, TZ_DATE_HOURS, 1, 4},
    {"setUTCDate", Builtin::DateSetUTC, TZ_DATE_DATE, 1, 1},
    {"setUTCMonth", Builtin::DateSetUTC, TZ_DATE_MONTH, 1, 2},
    {"setUTCFullYear", Builtin::DateSetUTC, TZ_DATE_YEAR, 1, 3},
    {"toISOString", Builtin::DateToISOString, 0, 0, 0},
    {"toJSON", Builtin::DateToJSON, 0, 0, 1},
    {"toUTCString", Builtin::DateToUTCString, 0, 0, 0},
    {"toString", Builtin::DateToLocalString, TZ_DATE_TEXT_FULL, 0, 0},
    {"toDateString", Builtin::DateToLocalString, TZ_DATE_TEXT_DATE, 0, 0},
    {"toTimeString", Builtin::DateToLocalString, TZ_DATE_TEXT_TIME, 0, 0},
    {"toLocaleString", Builtin::DateToLocale, TZ_DATE_LOCALE_ALL, 0, 2},
    {"toLocaleDateString", Builtin::DateToLocale, TZ_DATE_LOCALE_DATE, 0, 2},
    {"toLocaleTimeString", Builtin::DateToLocale, TZ_DATE_LOCALE_TIME, 0, 2},
};

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

DateChecker::DateChecker(SemanticState &owner)
    : state(owner) {}

Expr *DateChecker::dateTime(Expr *date) {
  auto *callee = this->state.getAst().make<MemberExpr>(date->loc(), date, "getTime");

  return ProcessChecker(this->state)
      .builtinCall(date->loc(), callee, Builtin::DateGetTime, {}, Type::intTy());
}

Expr *DateChecker::dateToString(Expr *date) {
  auto *callee = this->state.getAst().make<MemberExpr>(date->loc(), date, "toString");
  auto *mode = this->state.getAst().make<IntLitExpr>(date->loc(), TZ_DATE_TEXT_FULL);
  mode->type = Type::intTy();

  return ProcessChecker(this->state)
      .builtinCall(date->loc(), callee, Builtin::DateToLocalString, {mode}, Type::string());
}

void DateChecker::checkDateLiteral(const Expr *arg) {
  std::optional<std::u16string> text = DateChecker::literalText(arg);
  if (!text) {
    return;
  }
  ResultLiteralDate date = DateChecker::literalDate(*text);
  if (date.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), date.error().message);
  }
}

void DateChecker::checkDateFields(std::vector<Expr *> &args, size_t first, llvm::StringRef setter) {
  std::optional<int64_t> constant[7];
  for (size_t i = 0; i < args.size(); ++i) {
    args[i] = this->timeArg(args[i]);
    if (args[i]->type->isInteger()) {
      constant[first + i] = ConstantFolder(this->state).intConstant(args[i]);
    }
  }
  size_t end = first + args.size();
  auto report = [&](size_t field, const std::string &message) {
    this->state.getDiagnostics().error(args[field - first]->loc(), message);
  };
  bool shortYears = setter.empty();
  if (std::optional<int64_t> year = constant[0]; shortYears && year && *year >= 0 && *year <= 99) {
    std::string message = ErrorTextDateFields::shortYear(*year);
    report(0, message);
  }
  std::optional<int64_t> month = constant[1];
  if (month && (*month < 0 || *month > 11)) {
    std::string message =
        *month == 12 ? ErrorTextDateFields::month12() : ErrorTextDateFields::month(*month);
    report(1, message);
    month.reset();
  }
  if (std::optional<int64_t> day = constant[2]) {
    if (*day < 0) {
      std::string message = ErrorTextDateFields::dayBelow(*day);
      report(2, message);
    } else if (month) {
      std::optional<int64_t> year = constant[0];
      int64_t fullYear =
          shortYears && year && *year >= 0 && *year <= 99 ? 1900 + *year : year.value_or(0);
      int last = year          ? DateChecker::daysInMonth(fullYear, *month)
                 : *month == 1 ? 29
                               : DateChecker::daysInMonth(2001, *month);
      std::optional<int64_t> known = year ? std::optional<int64_t>(fullYear) : std::nullopt;
      if (*day > last) {
        std::string missing = DateChecker::missingDay(known, *month, *day);
        std::optional<int64_t> nextYear =
            year ? std::optional<int64_t>(*month == 11 ? fullYear + 1 : fullYear) : std::nullopt;
        int64_t next = *month == 11 ? 0 : *month + 1;
        std::string message = setter.empty()
                                  ? ErrorTextDateFields::lastDay(missing, *month, nextYear, next)
                              : *month == 1 ? ErrorTextDateFields::february(missing, setter, known)
                                            : ErrorTextDateFields::monthDays(missing, *month, last);
        report(2, message);
      }
    } else if (*day > 31) {
      std::string message = ErrorTextDateFields::dayAbove(*day);
      report(2, message);
    }
  }
  static const struct {
    const char *what;
    int64_t max;
  } limits[] = {{"hours", 23}, {"minutes", 59}, {"seconds", 59}, {"milliseconds", 999}};
  for (size_t i = std::max<size_t>(first, 3); i < end; ++i) {
    std::optional<int64_t> value = constant[i];
    if (value && (*value < 0 || *value > limits[i - 3].max)) {
      std::string message =
          ErrorTextDateFields::range(*value, limits[i - 3].what, limits[i - 3].max);
      report(i, message);
    }
  }
}

Expr *DateChecker::timeArg(Expr *arg) {
  if (!arg->type) {
    arg = ExprChecker(this->state).checkExpr(arg, Type::intTy());
  }
  arg = GenericChecker(this->state).apparent(arg);
  if (arg->type->isInteger()) {
    return arg->type->isInt() ? arg : TypeRelations(this->state).convert(arg, Type::intTy());
  }

  return TypeRelations(this->state).coerce(arg, Type::intTy());
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
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return failed();
  }
  if (args.empty()) {
    return ProcessChecker(this->state).builtinCall(loc, callee, Builtin::DateNew, {}, Type::date());
  }
  if (args.size() == 1) {
    this->checkDateLiteral(args[0]);
    Expr *arg = GenericChecker(this->state).apparent(ExprChecker(this->state).checkExpr(args[0]));
    const Type *type = arg->type;
    if (type->isError()) {
      return failed();
    }
    if (type->isDate()) {
      return ProcessChecker(this->state)
          .builtinCall(loc, callee, Builtin::DateCopy, {arg}, Type::date());
    }
    if (type->isString()) {
      if (!DateChecker::literalText(args[0])) {
        std::string message = ErrorTextDateNew::unknown();
        this->state.getDiagnostics().error(arg->loc(), message);
        return failed();
      }
      return ProcessChecker(this->state)
          .builtinCall(loc, callee, Builtin::DateFromString, {arg}, Type::date());
    }
    if (type->isNumeric()) {
      return ProcessChecker(this->state)
          .builtinCall(loc, callee, Builtin::DateNew, {this->timeArg(arg)}, Type::date());
    }
    std::string message = ErrorTextDateValue::takes(type);
    this->state.getDiagnostics().error(arg->loc(), message);
    return failed();
  }
  this->checkDateFields(args);

  return ProcessChecker(this->state)
      .builtinCall(loc, callee, Builtin::DateFromFields, args, Type::date());
}

Expr *DateChecker::checkDateFunction(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  if (!call->args.empty()) {
    std::string message = ErrorTextDateNew::call();
    this->state.getDiagnostics().error(name->loc(), message);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::DateText};
  call->type = Type::string();

  return call;
}

Expr *DateChecker::checkDateStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  llvm::StringRef name = member->name;
  if (name == "UTC") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 7);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    this->checkDateFields(call->args);
    call->target = BuiltinCall{.builtin = Builtin::DateUTC};
    call->type = Type::intTy();
    return call;
  }
  if (name == "parse") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    this->checkDateLiteral(call->args[0]);
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    call->target = BuiltinCall{.builtin = Builtin::DateParse};
    call->type = Type::result(Type::intTy(), Prelude(this->state).builtinType("DateError"));
    return call;
  }
  this->checkDateStaticMember(member);
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
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
  llvm::StringRef name = member->name;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }

    return call;
  };
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  const DateMethod *method = nullptr;
  for (const DateMethod &m : kMethods) {
    if (m.name == name) {
      method = &m;
    }
  }
  if (!method) {
    std::string message = name == "getYear"       ? ErrorTextDateMethod::getYear()
                          : name == "setYear"     ? ErrorTextDateMethod::setYear()
                          : name == "toGMTString" ? ErrorTextDateMethod::gmt()
                                                  : ErrorTextDateMethod::noMember(name);
    this->state.getDiagnostics().error(member->loc(), message);
    return loosely();
  }
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state).checkArity(call, method->min, method->max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto field = [&](int value) {
    auto *lit = this->state.getAst().make<IntLitExpr>(member->loc(), uint64_t(value));
    lit->type = Type::intTy();

    return lit;
  };
  call->target = BuiltinCall{.builtin = method->builtin};
  switch (method->builtin) {
  case Builtin::DateGetTime:
    call->type = Type::intTy();
    break;
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal:
    call->args.push_back(field(method->field));
    call->type = Type::intTy();
    break;
  case Builtin::DateSetTime:
    call->args[0] = this->timeArg(ExprChecker(this->state).checkExpr(call->args[0], Type::intTy()));
    call->type = Type::intTy();
    break;
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal:
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg, Type::intTy());
    }
    this->checkDateFields(
        call->args, size_t(method->field >= TZ_DATE_HOURS ? method->field - 1 : method->field),
        name);
    call->args.insert(call->args.begin(), field(method->field));
    call->type = Type::intTy();
    break;
  case Builtin::DateToJSON:
    if (!call->args.empty()) {
      call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    }
    call->type = Type::string();
    break;
  case Builtin::DateToLocalString:
    call->args.push_back(field(method->field));
    call->type = Type::string();
    break;
  case Builtin::DateToLocale:
    if (!DateFormatChecker(this->state).checkLocaleArgs(call->args, method->field)) {
      return call;
    }
    call->args.insert(call->args.begin(), field(method->field));
    call->type = Type::string();
    break;
  default:
    call->type = Type::string();
    break;
  }

  return call;
}

Expr *DateChecker::checkDateMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  bool method = llvm::any_of(kMethods, [&](const DateMethod &m) { return m.name == name; });
  std::string message = method              ? ErrorTextDateMethod::onlyCalled(name)
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
  bool compare = bin->op == BinaryOp::Lt || bin->op == BinaryOp::Le || bin->op == BinaryOp::Gt ||
                 bin->op == BinaryOp::Ge;
  if (l->isDate() && r->isDate() && (compare || bin->op == BinaryOp::Sub)) {
    bin->lhs = this->dateTime(bin->lhs);
    bin->rhs = this->dateTime(bin->rhs);
    bin->type = compare ? Type::boolTy() : Type::intTy();
    return bin;
  }
  const char *op = Operators::opSpelling(bin->op);
  std::string message = bin->op == BinaryOp::Add   ? ErrorTextDateOperator::plus()
                        : bin->op == BinaryOp::Sub ? ErrorTextDateOperator::minus()
                        : compare                  ? ErrorTextDateOperator::compare(op)
                                                   : BinaryChecker::operandTypesText(op, l, r);
  this->state.getDiagnostics().error(bin->loc(), message);

  return bin;
}

}
