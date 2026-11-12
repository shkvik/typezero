#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker_errors.h"

namespace tz {

std::string ErrorTextIntlOptions::noType() {
  return "Intl.DisplayNames needs the option 'type', e.g. '{ type: "
         "\"region\" }', which JavaScript would throw a TypeError "
         "without";
}

std::string ErrorTextIntlOptions::notObject(llvm::StringRef what, llvm::StringRef type) {
  return what.str() +
         " takes its options as an object, e.g. '{ style: \"percent\" "
         "}', found '" +
         type.str() + "'";
}

std::string ErrorTextIntlField::values(const std::vector<llvm::StringLiteral> &values) {
  auto out = std::string();
  for (size_t i = 0; i < values.size(); ++i) {
    out += (i == 0 ? "'" : i + 1 == values.size() ? " or '" : ", '") + values[i].str() + "'";
  }

  return out;
}

std::string ErrorTextIntlField::unknown(llvm::StringRef name, llvm::StringRef what,
                                        const IntlService *service) {
  auto names = std::string();
  for (size_t i = 0; i < service->options.size(); ++i) {
    names += (i == 0                             ? ""
              : i + 1 == service->options.size() ? " and "
                                                 : ", ") +
             service->options[i].name.str();
  }

  return "'" + name.str() + "' is not an option of " + what.str() + "; the options are " + names;
}

std::string ErrorTextIntlField::kind(llvm::StringRef name, char kind, llvm::StringRef type) {
  return "the option '" + name.str() + "' is " +
         (kind == 's'   ? "a string"
          : kind == 'n' ? "a number"
          : kind == 'b' ? "a boolean"
                        : "a string or a boolean") +
         ", found '" + type.str() + "'";
}

std::string ErrorTextIntlField::badValue(llvm::StringRef text, llvm::StringRef name,
                                         const std::vector<llvm::StringLiteral> &values) {
  return "'" + text.str() + "' is not a value of the option '" + name.str() +
         "', and JavaScript would throw a RangeError; it is " + ErrorTextIntlField::values(values);
}

std::string ErrorTextIntlField::open(llvm::StringRef name,
                                     const std::vector<llvm::StringLiteral> &values) {
  auto example = std::string();
  for (size_t i = 0; i < values.size() && i < 2; ++i) {
    example += (example.empty() ? "" : " || ") + std::string("x == \"") + values[i].str() + "\"";
  }

  return "the option '" + name.str() + "' is " + ErrorTextIntlField::values(values) +
         ", and a string known only when the program runs may be another, "
         "which JavaScript would throw a RangeError for; compare it with "
         "them first, e.g. 'if (" +
         example + ")'";
}

std::string ErrorTextIntlField::currency(llvm::StringRef code) {
  return "'" + code.str() +
         "' is not a currency code, and JavaScript would throw a RangeError; "
         "a code is three letters, e.g. 'RUB' or 'USD'";
}

std::string ErrorTextIntlField::unit(llvm::StringRef unit) {
  return "'" + unit.str() +
         "' is not a unit of Intl, and JavaScript would throw a RangeError; the "
         "units are those like 'kilometer', 'liter', 'percent' or 'megabyte', and "
         "two joined by '-per-', e.g. 'kilometer-per-hour'";
}

std::string ErrorTextIntlField::keyword(llvm::StringRef value, llvm::StringRef name) {
  return "'" + value.str() + "' is not a " + name.str() +
         ", and JavaScript would throw a RangeError; it is 3 to 8 letters or "
         "digits, e.g. '" +
         (name == "collation" ? "phonebk" : "latn") + "'";
}

std::string ErrorTextIntlField::styleCur() {
  return "style 'currency' needs the option 'currency', e.g. '{ style: "
         "\"currency\", currency: \"RUB\" }', which JavaScript would throw a "
         "TypeError without";
}

std::string ErrorTextIntlField::styleUnit() {
  return "style 'unit' needs the option 'unit', e.g. '{ style: \"unit\", unit: "
         "\"kilometer\" }', which JavaScript would throw a TypeError without";
}

std::string ErrorTextIntlField::range(llvm::StringRef name, int min, int max) {
  return name.str() + " is from " + std::to_string(min) + " to " + std::to_string(max) +
         ", and JavaScript would throw a RangeError";
}

std::string ErrorTextIntlField::rangeOpen(llvm::StringRef name, int min, int max) {
  std::string lo = std::to_string(min);
  std::string hi = std::to_string(max);

  return name.str() + " is from " + lo + " to " + hi +
         ", and a number known only when the program runs may be outside, "
         "which JavaScript would throw a RangeError for; give an int and "
         "check it first, e.g. 'if (n >= " +
         lo + " && n <= " + hi + ")'";
}

std::string ErrorTextIntlField::above(llvm::StringRef min, llvm::StringRef max, bool known) {
  return known ? min.str() + " is above " + max.str() + ", and JavaScript would throw a RangeError"
               : min.str() + " may be above " + max.str() +
                     ", which JavaScript would throw a RangeError for; check it "
                     "first, e.g. 'if (low <= high)'";
}

std::string ErrorTextIntlField::incSig() {
  return "roundingIncrement rounds to fraction digits, and with significant digits "
         "JavaScript would throw a TypeError";
}

std::string ErrorTextIntlField::incSame() {
  return "roundingIncrement needs minimumFractionDigits and maximumFractionDigits, and the "
         "same, e.g. '{ roundingIncrement: 5, minimumFractionDigits: 0, "
         "maximumFractionDigits: 0 }', or JavaScript may throw a RangeError";
}

std::string ErrorTextIntlField::increments(bool known) {
  return std::string("roundingIncrement is one of 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, "
                     "500, 1000, 2000, 2500 and 5000, and ") +
         (known ? "JavaScript would throw a RangeError"
                : "a number known only when the program runs may be another, which "
                  "JavaScript would throw a RangeError for; compare it with them first");
}

}
