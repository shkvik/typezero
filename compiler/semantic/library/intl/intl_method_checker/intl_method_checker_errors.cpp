#include "compiler/semantic/library/intl/intl_method_checker/intl_method_checker_errors.h"

namespace tz {

std::string ErrorTextIntlNumber::notNumber(llvm::StringRef method, const Type *type) {
  std::string message =
      "'" + method.str() + "' takes a number or a decimal, found '" + type->name().str() + "'";
  if (type->isOptional() && (type->inner()->isNumeric() || type->inner()->isDecimal())) {
    message += "; the value may be undefined: check it first";
  } else if (type->isString()) {
    message += "; read the number first, e.g. 'decimal(s)', which keeps every digit";
  }

  return message;
}

std::string ErrorTextClosedArg::unknown(llvm::StringRef what, llvm::StringRef first,
                                        llvm::StringRef second) {
  return "a string known only when the program runs may be no " + what.str() +
         ", which JavaScript would throw a RangeError for; compare it with them first, "
         "e.g. 'if (x == \"" +
         first.str() + "\" || x == \"" + second.str() + "\")'";
}

std::string ErrorTextClosedArg::notAllowed(llvm::StringRef text, llvm::StringRef what) {
  return "'" + text.str() + "' is no " + what.str() + ", and JavaScript would throw a RangeError";
}

std::string ErrorTextClosedArg::unit() {
  return "unit of time";
}

std::string ErrorTextClosedArg::form() {
  return "normalization form";
}

std::string ErrorTextIntlMethod::range(llvm::StringRef name) {
  return "'" + name.str() + "' is not supported yet; format both numbers";
}

std::string ErrorTextIntlMethod::selectSpan() {
  return "'selectRange' is not supported yet; select each number";
}

std::string ErrorTextIntlMethod::badUnit(llvm::StringRef unit) {
  return "'" + unit.str() +
         "' is not a unit of Intl.RelativeTimeFormat, and JavaScript would throw "
         "a RangeError; the units are year, quarter, month, week, day, hour, "
         "minute and second, also in the plural";
}

std::string ErrorTextIntlMethod::noMember(llvm::StringRef name, llvm::StringRef service) {
  return "property '" + name.str() + "' does not exist on type '" + service.str() + "'";
}

std::string ErrorTextIntlMethod::badForm(llvm::StringRef form) {
  return "'" + form.str() +
         "' is not a normalization form, and JavaScript would throw a "
         "RangeError; it is 'NFC', 'NFD', 'NFKC' or 'NFKD'";
}

}
