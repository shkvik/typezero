#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<std::u16string> literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

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

const char *const kMonthNames[] = {"January",   "February", "March",    "April",
                                   "May",       "June",     "July",     "August",
                                   "September", "October",  "November", "December"};

bool isLeap(int64_t year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

int daysInMonth(int64_t year, int64_t month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  return month == 1 && isLeap(year) ? 29 : days[month];
}

std::string missingDay(std::optional<int64_t> year, int64_t month, int64_t day) {
  std::string text = kMonthNames[month];
  if (year) {
    text += " " + std::to_string(*year);
  }
  text += " has no day " + std::to_string(day) + ", and JavaScript would ";
  if (!year) {
    return text + "move on to " + kMonthNames[(month + 1) % 12];
  }
  int64_t y = *year, m = month, d = day;
  while (d > daysInMonth(y, m)) {
    d -= daysInMonth(y, m);
    if (++m == 12) {
      m = 0;
      ++y;
    }
  }

  return text + "make it " + kMonthNames[m] + " " + std::to_string(d);
}

}

DateChecker::DateChecker(Checker &checker) : checker_(checker) {}

Expr *DateChecker::dateTime(Expr *date) {
  auto *callee = checker_.ctx_.make<MemberExpr>(date->loc(), date, "getTime");

  return checker_.processChecker().builtinCall(date->loc(), callee, Builtin::DateGetTime, {},
                                               Type::doubleTy());
}

Expr *DateChecker::dateToString(Expr *date) {
  auto *callee = checker_.ctx_.make<MemberExpr>(date->loc(), date, "toString");
  auto *mode = checker_.ctx_.make<IntLitExpr>(date->loc(), TZ_DATE_TEXT_FULL);
  mode->type = Type::intTy();

  return checker_.processChecker().builtinCall(date->loc(), callee, Builtin::DateToLocalString,
                                               {mode}, Type::string());
}

void DateChecker::checkDateLiteral(const Expr *arg) {
  std::optional<std::u16string> text = literalText(arg);
  if (!text) {
    return;
  }
  std::vector<uint16_t> units(text->begin(), text->end());
  TzDateFields fields;
  std::string shown = utf8(*text);
  if (!tz_date_parse_fields(units.data(), units.size(), &fields)) {
    checker_.diags_.error(arg->loc(),
                          "'" + shown +
                              "' is not a date JavaScript can read, and would be an Invalid "
                              "Date; write it as 'YYYY-MM-DDTHH:mm' (local time) or "
                              "'YYYY-MM-DDTHH:mmZ' (UTC), e.g. '2026-10-23T10:00'");
    return;
  }
  if (fields.date_only) {
    checker_.diags_.error(
        arg->loc(), "a date without a time is read as midnight UTC, while the same date "
                    "with a time is local; write '" +
                        shown + "T00:00' for local midnight or '" + shown + "T00:00Z' for UTC");
    return;
  }
  int64_t month = int64_t(fields.month), dayOfMonth = int64_t(fields.day);
  if (dayOfMonth > daysInMonth(int64_t(fields.year), month)) {
    checker_.diags_.error(
        arg->loc(), "'" + shown + "': " + missingDay(int64_t(fields.year), month, dayOfMonth) +
                        "; write the date that is meant");
    return;
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
    checker_.diags_.error(arg->loc(),
                          "'" + shown +
                              "' is outside the dates JavaScript has (100 million days around "
                              "1970), and would be an Invalid Date");
  }
}

void DateChecker::checkDateFields(std::vector<Expr *> &args) {
  std::optional<int64_t> constant[7];
  for (size_t i = 0; i < args.size(); ++i) {
    args[i] = checker_.mathChecker().numericArg(args[i]);
    if (args[i]->type->isInteger()) {
      constant[i] = checker_.constantFolder().intConstant(args[i]);
    }
    if (args[i]->type->isNumeric()) {
      args[i] = checker_.typeRelations().convert(args[i], Type::doubleTy());
    }
  }
  auto report = [&](size_t i, const std::string &message) {
    checker_.diags_.error(args[i]->loc(), message);
  };
  if (std::optional<int64_t> year = constant[0]; year && *year >= 0 && *year <= 99) {
    report(0, "the year " + std::to_string(*year) + " is " + std::to_string(1900 + *year) +
                  " in JavaScript, which reads the years 0 to 99 as 1900 to 1999; write the year "
                  "in full, or set a year before 100 with 'setFullYear'");
  }
  std::optional<int64_t> month = args.size() > 1 ? constant[1] : std::optional<int64_t>(0);
  if (month && (*month < 0 || *month > 11)) {
    if (*month == 12) {
      report(1, "month 12 is January of the next year in JavaScript: months are counted from 0, "
                "so December is 11");
    } else {
      report(1, "month " + std::to_string(*month) +
                    " is outside the year: months are counted from 0 (January) to 11 (December)");
    }
    month.reset();
  }
  if (std::optional<int64_t> day = args.size() > 2 ? constant[2] : std::nullopt) {
    if (*day < 0) {
      report(2, "day " + std::to_string(*day) +
                    " is outside the month: days are counted from 1, and 0 is the last day of the "
                    "month before");
    } else if (month) {
      std::optional<int64_t> year = constant[0];
      int64_t fullYear = year && *year >= 0 && *year <= 99 ? 1900 + *year : year.value_or(0);
      int last = year          ? daysInMonth(fullYear, *month)
                 : *month == 1 ? 29
                               : daysInMonth(2001, *month);
      if (*day > last) {
        std::string next = std::to_string(*month == 11 ? 0 : *month + 1);
        std::string nextYear = year ? std::to_string(*month == 11 ? fullYear + 1 : fullYear) : "y";
        report(2, missingDay(year ? std::optional<int64_t>(fullYear) : std::nullopt, *month, *day) +
                      "; the last day of " + kMonthNames[*month] +
                      " is day 0 of the month after: 'new Date(" + nextYear + ", " + next +
                      ", 0)'");
      }
    } else if (*day > 31) {
      report(2, "no month has day " + std::to_string(*day) + ": days are counted from 1 to 31");
    }
  }
  static const struct {
    const char *what;
    int64_t max;
  } limits[] = {{"hours", 23}, {"minutes", 59}, {"seconds", 59}, {"milliseconds", 999}};
  for (size_t i = 3; i < args.size(); ++i) {
    std::optional<int64_t> value = constant[i];
    if (value && (*value < 0 || *value > limits[i - 3].max)) {
      report(i, std::to_string(*value) + " " + limits[i - 3].what +
                    " are out of range: " + limits[i - 3].what + " are from 0 to " +
                    std::to_string(limits[i - 3].max) + " (JavaScript would carry the rest over)");
    }
  }
}

Expr *DateChecker::checkDateNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto *callee = checker_.ctx_.make<NameExpr>(loc, "Date");
  auto failed = [&] {
    auto *error = checker_.ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  if (args.size() > 7) {
    checker_.diags_.error(loc, "expected " + argumentCount(0, 7) + ", but got " +
                                   std::to_string(args.size()));
    for (Expr *&arg : args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return failed();
  }
  if (args.empty()) {
    return checker_.processChecker().builtinCall(loc, callee, Builtin::DateNew, {}, Type::date());
  }
  if (args.size() == 1) {
    checkDateLiteral(args[0]);
    Expr *arg = checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(args[0]));
    const Type *type = arg->type;
    if (type->isError()) {
      return failed();
    }
    if (type->isDate()) {
      return checker_.processChecker().builtinCall(loc, callee, Builtin::DateCopy, {arg},
                                                   Type::date());
    }
    if (type->isString()) {
      return checker_.processChecker().builtinCall(loc, callee, Builtin::DateFromString, {arg},
                                                   Type::date());
    }
    if (type->isNumeric()) {
      return checker_.processChecker().builtinCall(
          loc, callee, Builtin::DateNew, {checker_.typeRelations().convert(arg, Type::doubleTy())},
          Type::date());
    }
    std::string message = "'new Date' takes a time in milliseconds, a string or a date, found '" +
                          type->name().str() + "'";
    if (type->isOptional()) {
      message += "; the value may be undefined: check it first";
    } else if (type->isDecimal()) {
      message += "; convert it with 'Number(x)'";
    }
    checker_.diags_.error(arg->loc(), message);
    return failed();
  }
  checkDateFields(args);

  return checker_.processChecker().builtinCall(loc, callee, Builtin::DateFromFields, args,
                                               Type::date());
}

Expr *DateChecker::checkDateFunction(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (!call->args.empty()) {
    checker_.diags_.error(name->loc(),
                          "'Date()' without 'new' gives the text of the current time and "
                          "ignores its arguments; write 'new Date(...)'");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  call->builtin = Builtin::DateText;
  call->type = Type::string();

  return call;
}

Expr *DateChecker::checkDateStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  llvm::StringRef name = member->name;
  if (name == "UTC") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 7)) {
      return call;
    }
    checkDateFields(call->args);
    call->builtin = Builtin::DateUTC;
    call->type = Type::doubleTy();
    return call;
  }
  if (name == "parse") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    checkDateLiteral(call->args[0]);
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
    call->builtin = Builtin::DateParse;
    call->type = Type::doubleTy();
    return call;
  }
  checkDateStaticMember(member);
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

Expr *DateChecker::checkDateStaticMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (name == "now" || name == "UTC" || name == "parse") {
    checker_.diags_.error(member->loc(), "'Date." + name + "' can only be called: 'Date." + name +
                                             (name == "now"   ? "()'"
                                              : name == "UTC" ? "(y, m, d)'"
                                                              : "(s)'"));
  } else {
    checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on 'Date'");
  }

  return member;
}

Expr *DateChecker::checkDateMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  const DateMethod *method = nullptr;
  for (const DateMethod &m : kMethods) {
    if (m.name == name) {
      method = &m;
    }
  }
  if (!method) {
    if (name == "getYear") {
      checker_.diags_.error(member->loc(),
                            "'getYear' gives the year minus 1900 (126 for 2026); use "
                            "'getFullYear()'");
    } else if (name == "setYear") {
      checker_.diags_.error(member->loc(),
                            "'setYear' takes the year minus 1900, and reads 0 to 99 as "
                            "1900 to 1999; use 'setFullYear(year)'");
    } else if (name == "toGMTString") {
      checker_.diags_.error(member->loc(), "'toGMTString' is the old name of 'toUTCString'; use "
                                           "'toUTCString()'");
    } else {
      checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type 'Date'");
    }
    return loosely();
  }
  if (!checker_.argumentChecker().checkArity(call, member->loc(), method->min, method->max)) {
    return call;
  }
  auto field = [&](int value) {
    auto *lit = checker_.ctx_.make<IntLitExpr>(member->loc(), uint64_t(value));
    lit->type = Type::intTy();

    return lit;
  };
  call->builtin = method->builtin;
  switch (method->builtin) {
  case Builtin::DateGetTime:
    call->type = Type::doubleTy();
    break;
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal:
    call->args.push_back(field(method->field));
    call->type = Type::doubleTy();
    break;
  case Builtin::DateSetTime:
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal:
    for (Expr *&arg : call->args) {
      arg = checker_.mathChecker().numericArg(arg);
      if (arg->type->isNumeric()) {
        arg = checker_.typeRelations().convert(arg, Type::doubleTy());
      }
    }
    if (method->builtin != Builtin::DateSetTime) {
      call->args.insert(call->args.begin(), field(method->field));
    }
    call->type = Type::doubleTy();
    break;
  case Builtin::DateToJSON:
    if (!call->args.empty()) {
      call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
    }
    call->type = Type::optional(Type::string());
    break;
  case Builtin::DateToLocalString:
    call->args.push_back(field(method->field));
    call->type = Type::string();
    break;
  case Builtin::DateToLocale:
    if (!checker_.dateFormatChecker().checkLocaleArgs(call->args, method->field)) {
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
  if (llvm::any_of(kMethods, [&](const DateMethod &m) { return m.name == name; })) {
    checker_.diags_.error(member->loc(),
                          "'" + name + "' of a date can only be called, e.g. 'd." + name + "()'");
  } else if (name == "getYear") {
    checker_.diags_.error(member->loc(),
                          "'getYear' gives the year minus 1900; use 'getFullYear()'");
  } else {
    checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type 'Date'");
  }

  return member;
}

Expr *DateChecker::checkDateOperands(BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  if (!l->isDate() && !r->isDate()) {
    return nullptr;
  }
  bin->type = Type::error();
  bool compare = bin->op == BinaryOp::Lt || bin->op == BinaryOp::Le || bin->op == BinaryOp::Gt ||
                 bin->op == BinaryOp::Ge;
  if (l->isDate() && r->isDate() && (compare || bin->op == BinaryOp::Sub)) {
    bin->lhs = dateTime(bin->lhs);
    bin->rhs = dateTime(bin->rhs);
    bin->type = compare ? Type::boolTy() : Type::doubleTy();
    return bin;
  }
  const char *op = opSpelling(bin->op);
  if (bin->op == BinaryOp::Add) {
    checker_.diags_.error(bin->loc(),
                          "'+' with a date joins texts in JavaScript (the date as toString() "
                          "writes it); to move a date by milliseconds write 'new "
                          "Date(d.getTime() + ms)'");
  } else if (bin->op == BinaryOp::Sub) {
    checker_.diags_.error(bin->loc(),
                          "a date minus a number is a number of milliseconds in JavaScript, "
                          "not a date; write 'd.getTime() - ms', or 'new Date(d.getTime() - "
                          "ms)' for a date");
  } else if (compare) {
    checker_.diags_.error(bin->loc(), llvm::Twine("a date is compared with '") + op +
                                          "' only with another date; compare the times: "
                                          "'d.getTime() " +
                                          op + " ms'");
  } else {
    checker_.binaryChecker().reportOperandTypes(bin->loc(), op, l, r);
  }

  return bin;
}

}
