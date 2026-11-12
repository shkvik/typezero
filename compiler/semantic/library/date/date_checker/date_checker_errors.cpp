#include "compiler/semantic/library/date/date_checker/date_checker_errors.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker_errors.h"

namespace tz {

std::string ErrorTextDateDay::month(int64_t month) {
  static const char *const names[] = {"January",   "February", "March",    "April",
                                      "May",       "June",     "July",     "August",
                                      "September", "October",  "November", "December"};

  return names[month];
}

std::string ErrorTextDateDay::missing(std::optional<int64_t> year, int64_t month, int64_t day,
                                      int64_t toMonth, int64_t toDay) {
  auto text = ErrorTextDateDay::month(month);
  if (year) {
    text += " " + std::to_string(*year);
  }
  text += " has no day " + std::to_string(day) + ", and JavaScript would ";
  if (!year) {
    return text + "move on to " + ErrorTextDateDay::month((month + 1) % 12);
  }

  return text + "make it " + ErrorTextDateDay::month(toMonth) + " " + std::to_string(toDay);
}

std::string ErrorTextDateLiteral::unreadable(llvm::StringRef shown) {
  return "'" + shown.str() +
         "' is not a date JavaScript can read, and would be an Invalid "
         "Date; write it as 'YYYY-MM-DDTHH:mm' (local time) or "
         "'YYYY-MM-DDTHH:mmZ' (UTC), e.g. '2026-10-23T10:00'";
}

std::string ErrorTextDateLiteral::dateOnly(llvm::StringRef shown) {
  return "a date without a time is read as midnight UTC, while the same date "
         "with a time is local; write '" +
         shown.str() + "T00:00' for local midnight or '" + shown.str() + "T00:00Z' for UTC";
}

std::string ErrorTextDateLiteral::noDay(llvm::StringRef shown, llvm::StringRef missing) {
  return "'" + shown.str() + "': " + missing.str() + "; write the date that is meant";
}

std::string ErrorTextDateLiteral::outside(llvm::StringRef shown) {
  return "'" + shown.str() +
         "' is outside the dates JavaScript has (100 million days around "
         "1970), and would be an Invalid Date";
}

std::string ErrorTextDateFields::shortYear(int64_t year) {
  return "the year " + std::to_string(year) + " is " + std::to_string(1900 + year) +
         " in JavaScript, which reads the years 0 to 99 as 1900 to 1999; write the year "
         "in full, or set a year before 100 with 'setFullYear'";
}

std::string ErrorTextDateFields::month12() {
  return "month 12 is January of the next year in JavaScript: months are counted from 0, "
         "so December is 11";
}

std::string ErrorTextDateFields::month(int64_t month) {
  return "month " + std::to_string(month) +
         " is outside the year: months are counted from 0 (January) to 11 (December)";
}

std::string ErrorTextDateFields::dayBelow(int64_t day) {
  return "day " + std::to_string(day) +
         " is outside the month: days are counted from 1, and 0 is the last day of the "
         "month before";
}

std::string ErrorTextDateFields::february(llvm::StringRef missing, llvm::StringRef setter,
                                          std::optional<int64_t> year) {
  std::string yearPart = year ? std::to_string(*year) + ", " : "";

  return missing.str() + "; the last day of February is day 0 of March: '" + setter.str() + "(" +
         yearPart + "2, 0)'";
}

std::string ErrorTextDateFields::monthDays(llvm::StringRef missing, int64_t month, int64_t last) {
  return missing.str() + "; " + ErrorTextDateDay::month(month) + " has " + std::to_string(last) +
         " days";
}

std::string ErrorTextDateFields::lastDay(llvm::StringRef missing, int64_t month,
                                         std::optional<int64_t> nextYear, int64_t next) {
  std::string year = nextYear ? std::to_string(*nextYear) : "y";

  return missing.str() + "; the last day of " + ErrorTextDateDay::month(month) +
         " is day 0 of the month after: 'new Date(" + year + ", " + std::to_string(next) + ", 0)'";
}

std::string ErrorTextDateFields::dayAbove(int64_t day) {
  return "no month has day " + std::to_string(day) + ": days are counted from 1 to 31";
}

std::string ErrorTextDateFields::range(int64_t value, llvm::StringRef what, int64_t max) {
  return std::to_string(value) + " " + what.str() + " are out of range: " + what.str() +
         " are from 0 to " + std::to_string(max) + " (JavaScript would carry the rest over)";
}

std::string ErrorTextDateNew::arity(size_t count) {
  return "expected " + ErrorTextArity::wanted(0, 7) + ", but got " + std::to_string(count);
}

std::string ErrorTextDateNew::unknown() {
  return "'new Date' of a text known only when the program runs may fail, "
         "and 'new' gives no Result; write 'Date.parse(text)', which gives "
         "a Result with the milliseconds, and 'new Date(ms)'";
}

std::string ErrorTextDateNew::call() {
  return "'Date()' without 'new' gives the text of the current time and "
         "ignores its arguments; write 'new Date(...)'";
}

std::string ErrorTextDateStatic::onlyCalled(llvm::StringRef name) {
  return "'Date." + name.str() + "' can only be called: 'Date." + name.str() +
         (name == "now"   ? "()'"
          : name == "UTC" ? "(y, m, d)'"
                          : "(s)'");
}

std::string ErrorTextDateStatic::noMember(llvm::StringRef name) {
  return "property '" + name.str() + "' does not exist on 'Date'";
}

std::string ErrorTextDateMethod::getYear() {
  return "'getYear' gives the year minus 1900 (126 for 2026); use "
         "'getFullYear()'";
}

std::string ErrorTextDateMethod::setYear() {
  return "'setYear' takes the year minus 1900, and reads 0 to 99 as "
         "1900 to 1999; use 'setFullYear(year)'";
}

std::string ErrorTextDateMethod::gmt() {
  return "'toGMTString' is the old name of 'toUTCString'; use "
         "'toUTCString()'";
}

std::string ErrorTextDateMethod::noMember(llvm::StringRef name) {
  return "property '" + name.str() + "' does not exist on type 'Date'";
}

std::string ErrorTextDateMethod::onlyCalled(llvm::StringRef name) {
  return "'" + name.str() + "' of a date can only be called, e.g. 'd." + name.str() + "()'";
}

std::string ErrorTextDateMethod::getYearVal() {
  return "'getYear' gives the year minus 1900; use 'getFullYear()'";
}

std::string ErrorTextDateOperator::plus() {
  return "'+' with a date joins texts in JavaScript (the date as toString() "
         "writes it); to move a date by milliseconds write 'new "
         "Date(d.getTime() + ms)'";
}

std::string ErrorTextDateOperator::minus() {
  return "a date minus a number is a number of milliseconds in JavaScript, "
         "not a date; write 'd.getTime() - ms', or 'new Date(d.getTime() - "
         "ms)' for a date";
}

std::string ErrorTextDateOperator::compare(llvm::StringRef op) {
  return "a date is compared with '" + op.str() +
         "' only with another date; compare the times: "
         "'d.getTime() " +
         op.str() + " ms'";
}

std::string ErrorTextDateValue::takes(const Type *type) {
  std::string message = "'new Date' takes a time in milliseconds, a string or a date, found '" +
                        type->name().str() + "'";
  if (type->isOptional()) {
    message += "; the value may be undefined: check it first";
  } else if (type->isDecimal()) {
    message += "; convert it with 'Number(x)'";
  }

  return message;
}

}
