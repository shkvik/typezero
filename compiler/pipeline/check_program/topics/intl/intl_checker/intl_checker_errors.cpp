#include "compiler/pipeline/check_program/topics/intl/intl_checker/intl_checker_errors.h"

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

}
