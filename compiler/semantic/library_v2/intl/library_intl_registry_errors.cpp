#include "compiler/semantic/library_v2/intl/library_intl_registry_errors.h"
#include "runtime/library/date/date/date.h"

namespace tz {

std::string ErrorTextIntlNew::unknown(llvm::StringRef name) {
  return "'Intl." + name.str() +
         "' is not supported yet; Intl has DateTimeFormat, NumberFormat, "
         "Collator, PluralRules, RelativeTimeFormat, ListFormat, "
         "DisplayNames, Segmenter and Locale";
}

std::string ErrorTextIntlNew::withNew(llvm::StringRef name) {
  return "'Intl." + name.str() + "' is made with 'new': 'new Intl." + name.str() +
         "(...)', and JavaScript would throw a TypeError without it";
}

std::string ErrorTextIntlNew::noType() {
  return "Intl.DisplayNames needs the option 'type', e.g. 'new "
         "Intl.DisplayNames(\"ru\", { type: \"region\" })', which JavaScript "
         "would throw a TypeError without";
}

std::string ErrorTextIntlNew::label(llvm::StringRef name) {
  return "Intl." + name.str();
}

std::string ErrorTextLanguageTag::unknown(bool constructor, llvm::StringRef what) {
  return constructor
             ? std::string("a language known only when the program runs may be no language "
                           "tag, which JavaScript would throw a RangeError for; parse it with "
                           "'Intl.Locale.parse(tag)', which gives a Result")
             : "a language known only when the program runs may be no language tag, which "
               "JavaScript would throw a RangeError for; parse it first: "
               "'Intl.Locale.parse(tag)' gives a Result with an Intl.Locale, which " +
                   what.str() + " takes";
}

std::string ErrorTextLanguageTag::notTag(llvm::StringRef tag) {
  return "'" + tag.str() +
         "' is not a language tag, and JavaScript would throw a "
         "RangeError; write it as BCP 47 does, e.g. 'en-US' or 'ru'";
}

std::string ErrorTextLanguageTag::takes(llvm::StringRef what, llvm::StringRef type, bool array) {
  std::string message = what.str() +
                        " takes the language as a string, e.g. \"ru-RU\", or an Intl.Locale, "
                        "found '" +
                        type.str() + "'";
  if (array) {
    message += "; pass one language";
  }

  return message;
}

std::string ErrorTextLanguageTag::list(llvm::StringRef what, llvm::StringRef type) {
  return what.str() + " takes a language or an array of them, found '" + type.str() + "'";
}

std::string ErrorTextLanguageTag::listOpen(llvm::StringRef what) {
  return "languages known only when the program runs may be no language tags, "
         "which JavaScript would throw a RangeError for; parse each with "
         "'Intl.Locale.parse(tag)', which gives a Result, and pass them to " +
         what.str() + " one by one";
}

std::string ErrorTextLanguageTag::spread(llvm::StringRef what) {
  return what.str() + " takes the languages written in the array, not spread";
}

std::string ErrorTextIntlLocale::missing() {
  return "'Intl.Locale' is not available here";
}

std::string ErrorTextIntlLocale::options() {
  return "the options of Intl.Locale are not supported; write them into the "
         "tag, e.g. 'new Intl.Locale(\"th-TH-u-ca-buddhist\")'";
}

std::string ErrorTextIntlLocale::label() {
  return "Intl.Locale";
}

std::string ErrorTextIntlLocale::withNew() {
  return "'Intl.Locale' is made with 'new': 'new Intl.Locale(tag)' "
         "or 'Intl.Locale.parse(tag)', and JavaScript would throw "
         "a TypeError without it";
}

std::string ErrorTextIntlFunction::canonical() {
  return "Intl.getCanonicalLocales";
}

std::string ErrorTextIntlFunction::values() {
  return "'Intl.supportedValuesOf' is not supported yet";
}

std::string ErrorTextIntlFunction::staticOf(llvm::StringRef service, llvm::StringRef member) {
  return "Intl." + service.str() + "." + member.str();
}

std::string ErrorTextIntlFunction::onlyLocale(llvm::StringRef what) {
  return "'" + what.str() +
         "' is not supported; of the classes of Intl, only "
         "'supportedLocalesOf' is";
}

std::string ErrorTextIntlMember::dateFormat(llvm::StringRef name) {
  return "property '" + name.str() +
         "' of Intl.DateTimeFormat can only be called: 'format(d)', "
         "'formatToParts(d)' or 'resolvedOptions()'";
}

std::string ErrorTextIntlMember::onlyCalled(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' of " + type.str() + " can only be called, '" + name.str() +
         "(...)'; as values there are only 'format' of "
         "Intl.NumberFormat and Intl.DateTimeFormat and 'compare' "
         "of Intl.Collator";
}

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
             IntlOptionKeyTable::getName(service->options[i].key).str();
  }

  return "'" + name.str() + "' is not an option of " + what.str() + "; the options are " + names;
}

std::string ErrorTextIntlField::kind(llvm::StringRef name, IntlOptionKind kind,
                                     llvm::StringRef type) {
  return "the option '" + name.str() + "' is " + ErrorTextIntlField::kindName(kind) + ", found '" +
         type.str() + "'";
}

std::string ErrorTextIntlField::kindName(IntlOptionKind kind) {
  switch (kind) {
    case IntlOptionKind::TEXT:
      return "a string";
    case IntlOptionKind::NUMBER:
      return "a number";
    case IntlOptionKind::FLAG:
      return "a boolean";
    case IntlOptionKind::GROUPING:
      break;
  }

  return "a string or a boolean";
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
