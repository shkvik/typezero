#include "compiler/semantic/library/date/date_format_checker/date_format_checker_errors.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker_errors.h"
#include "runtime/library/date/date/date.h"

namespace tz {

std::string ErrorTextDateOptions::method(int kind) {
  if (kind == TZ_DATE_LOCALE_DATE) {
    return "toLocaleDateString";
  }
  if (kind == TZ_DATE_LOCALE_TIME) {
    return "toLocaleTimeString";
  }
  if (kind == TZ_DATE_LOCALE_FORMAT) {
    return "Intl.DateTimeFormat";
  }

  return "toLocaleString";
}

std::string ErrorTextDateOptions::notObject(llvm::StringRef method, llvm::StringRef type) {
  return method.str() +
         " takes its options as an object, e.g. '{ month: \"long\" "
         "}', found '" +
         type.str() + "'";
}

std::string ErrorTextDateOptions::unknown(llvm::StringRef name, llvm::StringRef method) {
  return "'" + name.str() + "' is not an option of " + method.str() +
         "; the options are weekday, era, year, month, day, "
         "dayPeriod, hour, minute, second, fractionalSecondDigits, "
         "timeZoneName, timeZone, hour12, hourCycle, dateStyle, "
         "timeStyle, calendar, numberingSystem, localeMatcher and "
         "formatMatcher";
}

std::string ErrorTextDateOptions::kind(llvm::StringRef name, bool boolean, bool number,
                                       llvm::StringRef type) {
  return "the option '" + name.str() + "' is " +
         (boolean  ? "a boolean"
          : number ? "a number"
                   : "a string") +
         ", found '" + type.str() + "'";
}

std::string ErrorTextDateOptions::styleOnly(llvm::StringRef name, llvm::StringRef method) {
  return "'" + name.str() + "' is not an option of " + method.str() +
         ", which JavaScript would throw a "
         "TypeError for; use toLocaleString";
}

std::string ErrorTextDateOptions::styleParts() {
  return "dateStyle and timeStyle cannot go with weekday, year, month, day, "
         "hour and the other parts, which JavaScript would throw a "
         "TypeError for; use either the styles or the parts";
}

std::string ErrorTextDateOptions::digitsName() {
  return "fractionalSecondDigits";
}

std::string ErrorTextDateOptions::digits() {
  return "fractionalSecondDigits known only when the program runs may "
         "be out of 1 to 3, which JavaScript would throw a RangeError "
         "for; give it the type '1 | 2 | 3'";
}

std::string ErrorTextDateOptions::unknownVal(llvm::StringRef name, llvm::StringRef value,
                                             llvm::StringRef first, llvm::StringRef second) {
  std::string x = value.empty() ? std::string("value") : value.str();

  return "the option '" + name.str() +
         "' known only when the program runs may be no value of it, "
         "which JavaScript would throw a RangeError for; check it "
         "first, e.g. 'if (" +
         x + " == \"" + first.str() + "\" || " + x + " == \"" + second.str() + "\")'";
}

std::string ErrorTextDateOptions::badValue(llvm::StringRef text, llvm::StringRef name,
                                           llvm::ArrayRef<llvm::StringLiteral> allowed) {
  auto values = std::string();
  for (size_t i = 0; i < allowed.size(); ++i) {
    values += (i == 0 ? "'" : i + 1 == allowed.size() ? " or '" : ", '") + allowed[i].str() + "'";
  }

  return "'" + text.str() + "' is not a value of the option '" + name.str() +
         "', and JavaScript would throw a "
         "RangeError; it is " +
         values;
}

std::string ErrorTextDateFormat::arity(size_t count) {
  return "expected " + ErrorTextArity::wanted(0, 2) + ", but got " + std::to_string(count);
}

std::string ErrorTextDateFormat::formatArg(llvm::StringRef name, llvm::StringRef type) {
  return "'" + name.str() + "' takes a date or a time in milliseconds, found '" + type.str() + "'";
}

std::string ErrorTextDateFormat::range(llvm::StringRef name) {
  return "'" + name.str() + "' is not supported yet; format both dates";
}

std::string ErrorTextDateFormat::noMember(llvm::StringRef name) {
  return "property '" + name.str() +
         "' does not exist on type "
         "'Intl.DateTimeFormat'";
}

}
