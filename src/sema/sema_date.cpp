// Dates (docs/date.md): Date as in JavaScript, a reference to an object that
// holds the time value, which every holder of the reference sees change;
// Date.UTC and Date.parse; the texts of local time and of languages
// (toLocaleString, Intl.DateTimeFormat).
//
// What goes wrong in JavaScript and is visible in the code is an error
// (section 5): a date string written in the code that JavaScript cannot read,
// or reads as UTC while the same date with a time would be local; February
// 31, month 12 and the like as numbers written in the code; getYear.

#include "sema/sema.h"

#include "../../runtime/date.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
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

// Text known when compiling: a literal, or a template without values.
std::optional<std::u16string> literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e))
    return lit->value;
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty())
    return tmpl->quasis[0];
  return std::nullopt;
}

struct DateMethod {
  llvm::StringLiteral name;
  Builtin builtin;
  int field; // TZ_DATE_* of a getter or a setter, the TZ_DATE_TEXT_* or TZ_DATE_LOCALE_* kind
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

// The options of Intl.DateTimeFormat (ECMA-402, 11.1.2): the strings, by
// their place among TZ_DTO_*, and the values each may have (none: any
// string); then hour12 and fractionalSecondDigits.
struct DateOption {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> values;
};

const DateOption kOptions[TZ_DTO_COUNT] = {
    {"localeMatcher", {"lookup", "best fit"}},
    {"calendar", {}},
    {"numberingSystem", {}},
    {"hourCycle", {"h11", "h12", "h23", "h24"}},
    {"timeZone", {}},
    {"weekday", {"narrow", "short", "long"}},
    {"era", {"narrow", "short", "long"}},
    {"year", {"2-digit", "numeric"}},
    {"month", {"2-digit", "numeric", "narrow", "short", "long"}},
    {"day", {"2-digit", "numeric"}},
    {"dayPeriod", {"narrow", "short", "long"}},
    {"hour", {"2-digit", "numeric"}},
    {"minute", {"2-digit", "numeric"}},
    {"second", {"2-digit", "numeric"}},
    {"timeZoneName", {"short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric"}},
    {"formatMatcher", {"basic", "best fit"}},
    {"dateStyle", {"full", "long", "medium", "short"}},
    {"timeStyle", {"full", "long", "medium", "short"}},
};

const char *const kMonthNames[] = {"January",   "February", "March",    "April",
                                   "May",       "June",     "July",     "August",
                                   "September", "October",  "November", "December"};

bool isLeap(int64_t year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

int daysInMonth(int64_t year, int64_t month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 1 && isLeap(year) ? 29 : days[month];
}

// "February 2026 has no day 31, and JavaScript would make it March 3"; of a
// year not known, "February has no day 30, and JavaScript would move on to
// March".
std::string missingDay(std::optional<int64_t> year, int64_t month, int64_t day) {
  std::string text = kMonthNames[month];
  if (year)
    text += " " + std::to_string(*year);
  text += " has no day " + std::to_string(day) + ", and JavaScript would ";
  if (!year)
    return text + "move on to " + kMonthNames[(month + 1) % 12];
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

const char *methodName(int kind) {
  switch (kind) {
  case TZ_DATE_LOCALE_DATE:
    return "toLocaleDateString";
  case TZ_DATE_LOCALE_TIME:
    return "toLocaleTimeString";
  case TZ_DATE_LOCALE_FORMAT:
    return "Intl.DateTimeFormat";
  default:
    return "toLocaleString";
  }
}

} // namespace

bool isLanguageTag(const std::string &tag) {
  auto alnum = [](char c) { return llvm::isAlnum(c); };
  auto alpha = [](char c) { return llvm::isAlpha(c); };
  llvm::SmallVector<llvm::StringRef, 4> parts;
  llvm::StringRef(tag).split(parts, '-');
  for (llvm::StringRef part : parts)
    if (part.empty() || part.size() > 8 || !llvm::all_of(part, alnum))
      return false;
  llvm::StringRef language = parts[0];
  bool length = (language.size() >= 2 && language.size() <= 3) ||
                (language.size() >= 5 && language.size() <= 8);
  return length && llvm::all_of(language, alpha);
}

Expr *Sema::dateTime(Expr *date) {
  auto *callee = ctx_.make<MemberExpr>(date->loc(), date, "getTime");
  return builtinCall(date->loc(), callee, Builtin::DateGetTime, {}, Type::doubleTy());
}

Expr *Sema::dateToString(Expr *date) {
  auto *callee = ctx_.make<MemberExpr>(date->loc(), date, "toString");
  auto *mode = ctx_.make<IntLitExpr>(date->loc(), TZ_DATE_TEXT_FULL);
  mode->type = Type::intTy();
  return builtinCall(date->loc(), callee, Builtin::DateToLocalString, {mode}, Type::string());
}

//===----------------------------------------------------------------------===//
// Making dates
//===----------------------------------------------------------------------===//

void Sema::checkDateLiteral(const Expr *arg) {
  std::optional<std::u16string> text = literalText(arg);
  if (!text)
    return;
  std::vector<uint16_t> units(text->begin(), text->end());
  TzDateFields fields;
  std::string shown = utf8(*text);
  if (!tz_date_parse_fields(units.data(), units.size(), &fields)) {
    diags_.error(arg->loc(), "'" + shown +
                                 "' is not a date JavaScript can read, and would be an Invalid "
                                 "Date; write it as 'YYYY-MM-DDTHH:mm' (local time) or "
                                 "'YYYY-MM-DDTHH:mmZ' (UTC), e.g. '2026-10-23T10:00'");
    return;
  }
  if (fields.date_only) {
    // "2026-10-23" is midnight UTC: in UTC+6 its getHours() is 6.
    diags_.error(arg->loc(), "a date without a time is read as midnight UTC, while the same date "
                             "with a time is local; write '" +
                                 shown + "T00:00' for local midnight or '" + shown +
                                 "T00:00Z' for UTC");
    return;
  }
  // "2026-02-30T10:00" is March 2.
  int64_t month = int64_t(fields.month), dayOfMonth = int64_t(fields.day);
  if (dayOfMonth > daysInMonth(int64_t(fields.year), month)) {
    diags_.error(arg->loc(), "'" + shown +
                                 "': " + missingDay(int64_t(fields.year), month, dayOfMonth) +
                                 "; write the date that is meant");
    return;
  }
  double day = tz_date_make_day(fields.year, fields.month, fields.day);
  double time = tz_date_make_time(fields.hour, fields.minute, fields.second, fields.ms);
  double value = tz_date_make_date(day, time);
  double limit = TZ_DATE_MAX_TIME;
  if (std::isnan(fields.offset))
    limit = TZ_DATE_MAX_LOCAL_TIME;
  else
    value -= fields.offset * 1000;
  if (!(value >= -limit && value <= limit))
    diags_.error(arg->loc(), "'" + shown +
                                 "' is outside the dates JavaScript has (100 million days around "
                                 "1970), and would be an Invalid Date");
}

void Sema::checkDateFields(std::vector<Expr *> &args) {
  std::optional<int64_t> constant[7];
  for (size_t i = 0; i < args.size(); ++i) {
    args[i] = numericArg(args[i]);
    if (args[i]->type->isInteger())
      constant[i] = intConstant(args[i]);
    if (args[i]->type->isNumeric())
      args[i] = convert(args[i], Type::doubleTy());
  }
  auto report = [&](size_t i, const std::string &message) {
    diags_.error(args[i]->loc(), message);
  };
  if (std::optional<int64_t> year = constant[0]; year && *year >= 0 && *year <= 99)
    report(0, "the year " + std::to_string(*year) + " is " + std::to_string(1900 + *year) +
                  " in JavaScript, which reads the years 0 to 99 as 1900 to 1999; write the year "
                  "in full, or set a year before 100 with 'setFullYear'");
  std::optional<int64_t> month = args.size() > 1 ? constant[1] : std::optional<int64_t>(0);
  if (month && (*month < 0 || *month > 11)) {
    if (*month == 12)
      report(1, "month 12 is January of the next year in JavaScript: months are counted from 0, "
                "so December is 11");
    else
      report(1, "month " + std::to_string(*month) +
                    " is outside the year: months are counted from 0 (January) to 11 (December)");
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
        // What JavaScript makes of it, and how the last day is written.
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
    if (value && (*value < 0 || *value > limits[i - 3].max))
      report(i, std::to_string(*value) + " " + limits[i - 3].what +
                    " are out of range: " + limits[i - 3].what + " are from 0 to " +
                    std::to_string(limits[i - 3].max) + " (JavaScript would carry the rest over)");
  }
}

// new Date(), new Date(ms), new Date(s), new Date(date) and new Date(y, m,
// d?, h?, min?, s?, ms?), the last in local time.
Expr *Sema::checkDateNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto *callee = ctx_.make<NameExpr>(loc, "Date");
  auto failed = [&] {
    auto *error = ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  };
  if (args.size() > 7) {
    diags_.error(loc,
                 "expected " + argumentCount(0, 7) + ", but got " + std::to_string(args.size()));
    for (Expr *&arg : args)
      arg = checkExpr(arg);
    return failed();
  }
  if (args.empty())
    return builtinCall(loc, callee, Builtin::DateNew, {}, Type::date());
  if (args.size() == 1) {
    checkDateLiteral(args[0]);
    Expr *arg = apparent(checkExpr(args[0]));
    const Type *type = arg->type;
    if (type->isError())
      return failed();
    if (type->isDate())
      return builtinCall(loc, callee, Builtin::DateCopy, {arg}, Type::date());
    if (type->isString())
      return builtinCall(loc, callee, Builtin::DateFromString, {arg}, Type::date());
    if (type->isNumeric())
      return builtinCall(loc, callee, Builtin::DateNew, {convert(arg, Type::doubleTy())},
                         Type::date());
    std::string message = "'new Date' takes a time in milliseconds, a string or a date, found '" +
                          type->name().str() + "'";
    if (type->isOptional())
      message += "; the value may be undefined: check it first";
    else if (type->isDecimal())
      message += "; convert it with 'Number(x)'";
    diags_.error(arg->loc(), message);
    return failed();
  }
  checkDateFields(args);
  return builtinCall(loc, callee, Builtin::DateFromFields, args, Type::date());
}

// `Date()` without `new`: the text of now, as toString() writes it.
Expr *Sema::checkDateFunction(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  if (!call->args.empty()) {
    // JavaScript ignores them, which hides what was meant.
    diags_.error(name->loc(), "'Date()' without 'new' gives the text of the current time and "
                              "ignores its arguments; write 'new Date(...)'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  call->builtin = Builtin::DateText;
  call->type = Type::string();
  return call;
}

// Date.UTC and Date.parse; Date.now is with the clocks (checkProcessCall).
Expr *Sema::checkDateStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  llvm::StringRef name = member->name;
  if (name == "UTC") {
    if (!checkArity(call, member->loc(), 1, 7))
      return call;
    checkDateFields(call->args);
    call->builtin = Builtin::DateUTC;
    call->type = Type::doubleTy();
    return call;
  }
  if (name == "parse") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    checkDateLiteral(call->args[0]);
    call->args[0] = checkArg(call->args[0], Type::string());
    call->builtin = Builtin::DateParse;
    call->type = Type::doubleTy();
    return call;
  }
  checkDateStaticMember(member);
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

Expr *Sema::checkDateStaticMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (name == "now" || name == "UTC" || name == "parse")
    diags_.error(member->loc(), "'Date." + name + "' can only be called: 'Date." + name +
                                    (name == "now"   ? "()'"
                                     : name == "UTC" ? "(y, m, d)'"
                                                     : "(s)'"));
  else
    diags_.error(member->loc(), "property '" + name + "' does not exist on 'Date'");
  return member;
}

//===----------------------------------------------------------------------===//
// Methods
//===----------------------------------------------------------------------===//

Expr *Sema::checkDateMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  if (rejectSpread(call))
    return call;
  const DateMethod *method = nullptr;
  for (const DateMethod &m : kMethods)
    if (m.name == name)
      method = &m;
  if (!method) {
    if (name == "getYear")
      diags_.error(member->loc(), "'getYear' gives the year minus 1900 (126 for 2026); use "
                                  "'getFullYear()'");
    else if (name == "setYear")
      diags_.error(member->loc(), "'setYear' takes the year minus 1900, and reads 0 to 99 as "
                                  "1900 to 1999; use 'setFullYear(year)'");
    else if (name == "toGMTString")
      diags_.error(member->loc(), "'toGMTString' is the old name of 'toUTCString'; use "
                                  "'toUTCString()'");
    else
      diags_.error(member->loc(), "property '" + name + "' does not exist on type 'Date'");
    return loosely();
  }
  if (!checkArity(call, member->loc(), method->min, method->max))
    return call;
  auto field = [&](int value) {
    auto *lit = ctx_.make<IntLitExpr>(member->loc(), uint64_t(value));
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
      arg = numericArg(arg);
      if (arg->type->isNumeric())
        arg = convert(arg, Type::doubleTy());
    }
    if (method->builtin != Builtin::DateSetTime)
      call->args.insert(call->args.begin(), field(method->field));
    call->type = Type::doubleTy();
    break;
  case Builtin::DateToJSON:
    // The key JSON.stringify passes, which a date ignores.
    if (!call->args.empty())
      call->args[0] = checkArg(call->args[0], Type::string());
    // null in JavaScript for an invalid date: there is no null.
    call->type = Type::optional(Type::string());
    break;
  case Builtin::DateToLocalString:
    call->args.push_back(field(method->field));
    call->type = Type::string();
    break;
  case Builtin::DateToLocale:
    if (!checkLocaleArgs(call->args, method->field))
      return call;
    call->args.insert(call->args.begin(), field(method->field));
    call->type = Type::string();
    break;
  default: // toISOString, toUTCString
    call->type = Type::string();
    break;
  }
  return call;
}

Expr *Sema::checkDateMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (llvm::any_of(kMethods, [&](const DateMethod &m) { return m.name == name; }))
    diags_.error(member->loc(),
                 "'" + name + "' of a date can only be called, e.g. 'd." + name + "()'");
  else if (name == "getYear")
    diags_.error(member->loc(), "'getYear' gives the year minus 1900; use 'getFullYear()'");
  else
    diags_.error(member->loc(), "property '" + name + "' does not exist on type 'Date'");
  return member;
}

// `a - b` (the milliseconds between two dates) and `a < b`: the time values.
// Null if neither side is a date.
Expr *Sema::checkDateOperands(BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  if (!l->isDate() && !r->isDate())
    return nullptr;
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
  if (bin->op == BinaryOp::Add)
    diags_.error(bin->loc(), "'+' with a date joins texts in JavaScript (the date as toString() "
                             "writes it); to move a date by milliseconds write 'new "
                             "Date(d.getTime() + ms)'");
  else if (bin->op == BinaryOp::Sub)
    diags_.error(bin->loc(), "a date minus a number is a number of milliseconds in JavaScript, "
                             "not a date; write 'd.getTime() - ms', or 'new Date(d.getTime() - "
                             "ms)' for a date");
  else if (compare)
    diags_.error(bin->loc(), llvm::Twine("a date is compared with '") + op +
                                 "' only with another date; compare the times: "
                                 "'d.getTime() " +
                                 op + " ms'");
  else
    reportOperandTypes(bin->loc(), op, l, r);
  return bin;
}

//===----------------------------------------------------------------------===//
// Languages
//===----------------------------------------------------------------------===//

// The locale and the options, both optional, of toLocaleString and the like
// and of Intl.DateTimeFormat. The locale is a string or undefined; the
// options an object whose fields are options, each as its type is (a string,
// hour12 a boolean, fractionalSecondDigits a number), or that type or
// undefined. What JavaScript rejects with an exception when the program runs
// and is written in the code is an error here.
bool Sema::checkLocaleArgs(std::vector<Expr *> &args, int kind) {
  bool ok = true;
  if (!args.empty()) {
    std::optional<std::u16string> tag = literalText(args[0]);
    args[0] = checkExpr(args[0]);
    const Type *type = args[0]->type;
    if (type->isError()) {
      ok = false;
    } else if (!type->isUndefined() && type != Type::string() &&
               type != Type::optional(Type::string())) {
      std::string message = std::string(methodName(kind)) +
                            " takes the language as a string, e.g. \"ru-RU\", found '" +
                            type->name().str() + "'";
      if (type->isArray())
        message += "; pass one language";
      diags_.error(args[0]->loc(), message);
      ok = false;
    } else if (tag && !isLanguageTag(utf8(*tag))) {
      diags_.error(args[0]->loc(), "'" + utf8(*tag) +
                                       "' is not a language tag, and JavaScript would throw a "
                                       "RangeError; write it as BCP 47 does, e.g. 'en-US' or 'ru'");
      ok = false;
    }
  }
  if (args.size() < 2)
    return ok;
  Expr *&options = args[1];
  options = checkExpr(options);
  const Type *type = options->type;
  if (type->isError())
    return false;
  if (type->isUndefined())
    return ok;
  if (!type->isObject() || type->isTuple() || type->isClass() || type->isRegExp() ||
      type->isMatch() || type->isDate() || type->isIntl() || type->isCrypto() ||
      type->isBytes()) {
    diags_.error(options->loc(), std::string(methodName(kind)) +
                                     " takes its options as an object, e.g. '{ month: \"long\" "
                                     "}', found '" +
                                     type->name().str() + "'");
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  bool hasStyle = false, hasComponent = false;
  for (const Field &field : type->fields()) {
    const Type *value = field.type->nonOptional();
    auto found = llvm::find_if(kOptions, [&](const DateOption &o) { return o.name == field.name; });
    const Type *wanted = Type::string();
    if (field.name == "hour12")
      wanted = Type::boolTy();
    else if (field.name == "fractionalSecondDigits")
      wanted = value->isNumeric() ? value : Type::intTy();
    else if (found == std::end(kOptions)) {
      diags_.error(options->loc(), "'" + field.name + "' is not an option of " + methodName(kind) +
                                       "; the options are weekday, era, year, month, day, "
                                       "dayPeriod, hour, minute, second, fractionalSecondDigits, "
                                       "timeZoneName, timeZone, hour12, hourCycle, dateStyle, "
                                       "timeStyle, calendar, numberingSystem, localeMatcher and "
                                       "formatMatcher");
      ok = false;
      continue;
    }
    if (value != wanted) {
      diags_.error(options->loc(), "the option '" + field.name + "' is " +
                                       (wanted == Type::boolTy() ? "a boolean"
                                        : wanted->isNumeric()    ? "a number"
                                                                 : "a string") +
                                       ", found '" + field.type->name().str() + "'");
      ok = false;
      continue;
    }
    // What must be there, for the conflicts below.
    bool present = !field.type->isOptional();
    if (field.name == "dateStyle" || field.name == "timeStyle") {
      hasStyle |= present;
      if (present && ((kind == TZ_DATE_LOCALE_DATE && field.name == "timeStyle") ||
                      (kind == TZ_DATE_LOCALE_TIME && field.name == "dateStyle"))) {
        diags_.error(options->loc(), "'" + field.name + "' is not an option of " +
                                         methodName(kind) +
                                         ", which JavaScript would throw a "
                                         "TypeError for; use toLocaleString");
        ok = false;
      }
    } else if (field.name != "hour12" && field.name != "hourCycle" && field.name != "timeZone" &&
               field.name != "calendar" && field.name != "numberingSystem" &&
               field.name != "localeMatcher" && field.name != "formatMatcher") {
      hasComponent |= present;
    }
  }
  if (hasStyle && hasComponent) {
    diags_.error(options->loc(),
                 "dateStyle and timeStyle cannot go with weekday, year, month, day, "
                 "hour and the other parts, which JavaScript would throw a "
                 "TypeError for; use either the styles or the parts");
    ok = false;
  }
  // The values written in a literal.
  if (literal) {
    for (const ObjectLitExpr::Property &prop : literal->props) {
      if (prop.spread)
        continue;
      auto found =
          llvm::find_if(kOptions, [&](const DateOption &o) { return o.name == prop.name; });
      std::optional<std::u16string> text = prop.value ? literalText(prop.value) : std::nullopt;
      if (found != std::end(kOptions) && text && !found->values.empty() &&
          !llvm::is_contained(found->values, utf8(*text))) {
        std::string values;
        for (size_t i = 0; i < found->values.size(); ++i)
          values += (i == 0                          ? "'"
                     : i + 1 == found->values.size() ? " or '"
                                                     : ", '") +
                    found->values[i].str() + "'";
        diags_.error(prop.value->loc(), "'" + utf8(*text) + "' is not a value of the option '" +
                                            prop.name +
                                            "', and JavaScript would throw a "
                                            "RangeError; it is " +
                                            values);
        ok = false;
      }
      if (prop.name == "fractionalSecondDigits" && prop.value)
        if (std::optional<int64_t> digits = intConstant(prop.value);
            digits && (*digits < 1 || *digits > 3)) {
          diags_.error(prop.value->loc(), "fractionalSecondDigits is from 1 to 3, and JavaScript "
                                          "would throw a RangeError");
          ok = false;
        }
    }
  }
  return ok;
}

const Type *Sema::dateOptionsType() {
  std::vector<Field> fields;
  for (const DateOption &option : kOptions)
    fields.push_back({option.name.str(), Type::optional(Type::string()), false});
  fields.push_back({"hour12", Type::optional(Type::boolTy()), false});
  fields.push_back({"fractionalSecondDigits", Type::optional(Type::intTy()), false});
  return Type::object(std::move(fields), "Intl.DateTimeFormatOptions");
}

const Type *Sema::dateFormatPartType() {
  return Type::object({{"type", Type::string(), false}, {"value", Type::string(), false}},
                      "Intl.DateTimeFormatPart");
}

// What resolvedOptions() gives, with the fields in the order node prints
// them.
const Type *Sema::resolvedDateOptionsType() {
  const Type *text = Type::string(), *maybe = Type::optional(Type::string());
  return Type::object({{"locale", text, false},
                       {"calendar", text, false},
                       {"numberingSystem", text, false},
                       {"timeZone", text, false},
                       {"hourCycle", maybe, false},
                       {"hour12", Type::optional(Type::boolTy()), false},
                       {"weekday", maybe, false},
                       {"era", maybe, false},
                       {"year", maybe, false},
                       {"month", maybe, false},
                       {"day", maybe, false},
                       {"dayPeriod", maybe, false},
                       {"hour", maybe, false},
                       {"minute", maybe, false},
                       {"second", maybe, false},
                       {"fractionalSecondDigits", Type::optional(Type::intTy()), false},
                       {"timeZoneName", maybe, false},
                       {"dateStyle", maybe, false},
                       {"timeStyle", maybe, false}},
                      "Intl.ResolvedDateTimeFormatOptions");
}

// new Intl.DateTimeFormat(locale?, options?), also without `new`.
Expr *Sema::checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto *callee = ctx_.make<NameExpr>(loc, "Intl");
  if (args.size() > 2) {
    diags_.error(loc,
                 "expected " + argumentCount(0, 2) + ", but got " + std::to_string(args.size()));
    for (Expr *&arg : args)
      arg = checkExpr(arg);
    auto *error = ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  if (!checkLocaleArgs(args, TZ_DATE_LOCALE_FORMAT)) {
    auto *error = ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  return builtinCall(loc, callee, Builtin::DtfNew, args, Type::dateFormat());
}

// `Intl.DateTimeFormat(...)` without `new`; the rest of Intl is in
// sema_intl.cpp.
Expr *Sema::checkIntlCall(CallExpr *call, MemberExpr *member) {
  if (member->name == "DateTimeFormat") {
    if (rejectSpread(call))
      return call;
    return checkDateFormatNew(member->loc(), call->args);
  }
  return checkIntlFunction(call, member);
}

Expr *Sema::checkDateFormatMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  if (name == "format" || name == "formatToParts") {
    if (!checkArity(call, member->loc(), 0, 1))
      return call;
    // The date, or its time in milliseconds; now without one.
    if (!call->args.empty()) {
      Expr *&arg = call->args[0];
      arg = apparent(checkExpr(arg));
      if (arg->type->isNumeric()) {
        arg = convert(arg, Type::doubleTy());
      } else if (!arg->type->isDate() && !arg->type->isError()) {
        diags_.error(arg->loc(), "'" + name + "' takes a date or a time in milliseconds, found '" +
                                     arg->type->name() + "'");
        return call;
      }
    }
    call->builtin = name == "format" ? Builtin::DtfFormat : Builtin::DtfFormatToParts;
    call->type = name == "format" ? Type::string() : Type::array(dateFormatPartType());
    return call;
  }
  if (name == "resolvedOptions") {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    call->builtin = Builtin::DtfResolvedOptions;
    call->type = resolvedDateOptionsType();
    return call;
  }
  if (name == "formatRange" || name == "formatRangeToParts")
    diags_.error(member->loc(), "'" + name + "' is not supported yet; format both dates");
  else
    diags_.error(member->loc(), "property '" + name +
                                    "' does not exist on type "
                                    "'Intl.DateTimeFormat'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

} // namespace tz
