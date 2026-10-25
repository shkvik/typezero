#include "compiler/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "compiler/whole_program/flows/flows.h"
#include "runtime/library/intl/intl/intl.h"
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

// Text known when compiling: a literal, or a template without values.
std::optional<std::u16string> literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

// A number written in the code: a literal, or one negated.
std::optional<double> literalNumber(const Expr *e) {
  if (auto *lit = dyn_cast<IntLitExpr>(e)) {
    return double(lit->value);
  }
  if (auto *lit = dyn_cast<FloatLitExpr>(e)) {
    return lit->value;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(e); unary && unary->op == UnaryOp::Neg) {
    if (std::optional<double> value = literalNumber(unary->operand)) {
      return -*value;
    }
  }

  return std::nullopt;
}

const IntlService *serviceOf(IntlKind kind) {
  for (const IntlService &s : services()) {
    if (s.kind == kind) {
      return &s;
    }
  }

  return nullptr;
}

// The options of supportedLocalesOf.
const IntlService kSupported = {IntlKind::None, "supportedLocalesOf", 0, {kMatcher}, true};

// The units of ECMA-402 (IsSanctionedSingleUnitIdentifier), and two joined
// by "-per-".
bool isSanctionedUnit(llvm::StringRef unit) {
  static const llvm::StringLiteral units[] = {
      "acre",        "bit",         "byte",        "celsius",
      "centimeter",  "day",         "degree",      "fahrenheit",
      "fluid-ounce", "foot",        "gallon",      "gigabit",
      "gigabyte",    "gram",        "hectare",     "hour",
      "inch",        "kilobit",     "kilobyte",    "kilogram",
      "kilometer",   "liter",       "megabit",     "megabyte",
      "meter",       "microsecond", "mile",        "mile-scandinavian",
      "milliliter",  "millimeter",  "millisecond", "minute",
      "month",       "nanosecond",  "ounce",       "percent",
      "petabyte",    "pound",       "second",      "stone",
      "terabit",     "terabyte",    "week",        "yard",
      "year"};

  return llvm::is_contained(units, unit);
}

bool isWellFormedUnit(llvm::StringRef unit) {
  if (isSanctionedUnit(unit)) {
    return true;
  }
  auto [numerator, denominator] = unit.split("-per-");

  return !denominator.empty() && isSanctionedUnit(numerator) && isSanctionedUnit(denominator);
}

bool isKeywordType(llvm::StringRef text) {
  llvm::SmallVector<llvm::StringRef, 4> parts;
  text.split(parts, '-');

  return llvm::all_of(parts, [](llvm::StringRef part) {
    return part.size() >= 3 && part.size() <= 8 && llvm::all_of(part, llvm::isAlnum);
  });
}

// The values of an option in a message: "'a', 'b' or 'c'".
std::string valueList(const std::vector<llvm::StringLiteral> &values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    out += (i == 0 ? "'" : i + 1 == values.size() ? " or '" : ", '") + values[i].str() + "'";
  }

  return out;
}

// What the string of a field holds, if the code says: a literal, or a
// string literal type.
std::optional<std::string> knownText(const Expr *value, const Type *type) {
  if (std::optional<std::u16string> text = value ? literalText(value) : std::nullopt) {
    return utf8(*text);
  }
  if (type && type->isStringLiteral()) {
    return utf8(type->literalText());
  }

  return std::nullopt;
}

bool isStringLike(const Type *type) {
  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() &&
          llvm::all_of(type->members(), [](const Type *m) { return m->isStringLiteral(); }));
}

} // namespace

IntlOptionsChecker::IntlOptionsChecker(Checker &checker) : checker_(checker) {}

// The locale and the options at args[from] and args[from + 1], both
// optional. The locale is a string or undefined; the options an object
// whose fields are options of the service, each of its type or that type or
// undefined. What JavaScript rejects with an exception and is written in the
// code is an error. False after an error.
bool IntlOptionsChecker::checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind,
                                       llvm::StringRef what) {
  const IntlService *service = kind == IntlKind::None ? &kSupported : serviceOf(kind);
  bool ok = true;
  if (args.size() > from) {
    Expr *&locale = args[from];
    std::optional<std::u16string> tag = literalText(locale);
    locale = checker_.exprChecker().checkExpr(locale);
    const Type *type = locale->type;
    if (type->isError()) {
      ok = false;
    } else if (!type->isUndefined() && type != Type::string() &&
               type != Type::optional(Type::string())) {
      std::string message = what.str() +
                            " takes the language as a string, e.g. \"ru-RU\", found '" +
                            type->name().str() + "'";
      if (type->isArray()) {
        message += "; pass one language";
      }
      checker_.diags_.error(locale->loc(), message);
      ok = false;
    } else if (tag && !isLanguageTag(utf8(*tag))) {
      checker_.diags_.error(locale->loc(),
                            "'" + utf8(*tag) +
                                "' is not a language tag, and JavaScript would throw a "
                                "RangeError; write it as BCP 47 does, e.g. 'en-US' or 'ru'");
      ok = false;
    }
  }
  if (args.size() <= from + 1) {
    return ok;
  }
  Expr *&options = args[from + 1];
  options = checker_.exprChecker().checkExpr(options);
  const Type *type = options->type;
  if (type->isError()) {
    return false;
  }
  if (type->isUndefined()) {
    if (kind == IntlKind::DisplayNames) {
      checker_.diags_.error(options->loc(),
                            "Intl.DisplayNames needs the option 'type', e.g. '{ type: "
                            "\"region\" }', which JavaScript would throw a TypeError "
                            "without");
      return false;
    }
    return ok;
  }
  if (!type->isObject() || type->isTuple() || type->isClass() || type->isRegExp() ||
      type->isMatch() || type->isDate() || type->isIntl() || type->isCrypto() || type->isBytes()) {
    checker_.diags_.error(options->loc(),
                          what.str() +
                              " takes its options as an object, e.g. '{ style: \"percent\" "
                              "}', found '" +
                              type->name().str() + "'");
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  auto valueOf = [&](llvm::StringRef name) -> Expr * {
    if (!literal) {
      return nullptr;
    }
    for (const ObjectLitExpr::Property &prop : literal->props) {
      if (!prop.spread && prop.name == name) {
        return prop.value;
      }
    }

    return nullptr;
  };
  // What a field holds, if the code says.
  auto textOf = [&](llvm::StringRef name) -> std::optional<std::string> {
    const Field *field = type->field(name);

    return field ? knownText(valueOf(name), field->type->nonOptional()) : std::nullopt;
  };
  auto numberOf = [&](llvm::StringRef name) -> std::optional<double> {
    Expr *value = valueOf(name);

    return value ? literalNumber(value) : std::nullopt;
  };
  auto report = [&](llvm::StringRef name, const std::string &message) {
    Expr *value = valueOf(name);
    checker_.diags_.error(value ? value->loc() : options->loc(), message);
    ok = false;
  };

  std::string names;
  for (size_t i = 0; i < service->options.size(); ++i) {
    names += (i == 0                             ? ""
              : i + 1 == service->options.size() ? " and "
                                                 : ", ") +
             service->options[i].name.str();
  }
  for (const Field &field : type->fields()) {
    const Type *value = field.type->nonOptional();
    auto found =
        llvm::find_if(service->options, [&](const IntlOption &o) { return o.name == field.name; });
    if (found == service->options.end()) {
      report(field.name, "'" + field.name + "' is not an option of " + what.str() +
                             "; the options are " + names);
      continue;
    }
    if (value->isUndefined()) { // as if not given
      continue;
    }
    auto isBoolean = [](const Type *t) {
      return t->isBool() || (t->isLiteral() && t->literalBase()->isBool());
    };
    bool fits = found->kind == 's'   ? isStringLike(value)
                : found->kind == 'n' ? value->isNumeric()
                : found->kind == 'b'
                    ? isBoolean(value)
                    : isStringLike(value) || isBoolean(value) ||
                          (value->isUnion() && llvm::all_of(value->members(), [&](const Type *m) {
                             return isStringLike(m) || isBoolean(m);
                           }));
    if (!fits) {
      report(field.name, "the option '" + field.name + "' is " +
                             (found->kind == 's'   ? "a string"
                              : found->kind == 'n' ? "a number"
                              : found->kind == 'b' ? "a boolean"
                                                   : "a string or a boolean") +
                             ", found '" + field.type->name().str() + "'");
      continue;
    }
    // A value written in the code that is not one of the option.
    if (std::optional<std::string> text = textOf(field.name);
        text && !found->values.empty() && !llvm::is_contained(found->values, *text) &&
        !(found->kind == 'g' && text->empty())) {
      report(field.name, "'" + *text + "' is not a value of the option '" + field.name +
                             "', and JavaScript would throw a RangeError; it is " +
                             valueList(found->values));
      continue;
    }
  }
  if (!ok) {
    return false;
  }

  // What JavaScript checks of the values themselves.
  if (std::optional<std::string> code = textOf("currency");
      code && !(code->size() == 3 && llvm::all_of(*code, llvm::isAlpha))) {
    report("currency", "'" + *code +
                           "' is not a currency code, and JavaScript would throw a RangeError; "
                           "a code is three letters, e.g. 'RUB' or 'USD'");
  }
  if (std::optional<std::string> unit = textOf("unit"); unit && !isWellFormedUnit(*unit)) {
    report("unit", "'" + *unit +
                       "' is not a unit of Intl, and JavaScript would throw a RangeError; the "
                       "units are those like 'kilometer', 'liter', 'percent' or 'megabyte', and "
                       "two joined by '-per-', e.g. 'kilometer-per-hour'");
  }
  for (const char *name : {"numberingSystem", "collation"}) {
    if (std::optional<std::string> value = textOf(name); value && !isKeywordType(*value)) {
      report(name, "'" + *value + "' is not a " + name +
                       ", and JavaScript would throw a RangeError; it is 3 to 8 letters or "
                       "digits, e.g. '" +
                       (llvm::StringRef(name) == "collation" ? "phonebk" : "latn") + "'");
    }
  }
  if (std::optional<std::string> style = textOf("style"); style && kind == IntlKind::NumberFormat) {
    if (*style == "currency" && !type->field("currency")) {
      report("style", "style 'currency' needs the option 'currency', e.g. '{ style: "
                      "\"currency\", currency: \"RUB\" }', which JavaScript would throw a "
                      "TypeError without");
    }
    if (*style == "unit" && !type->field("unit")) {
      report("style", "style 'unit' needs the option 'unit', e.g. '{ style: \"unit\", unit: "
                      "\"kilometer\" }', which JavaScript would throw a TypeError without");
    }
  }
  if (kind == IntlKind::DisplayNames && !type->field("type")) {
    report("type", "Intl.DisplayNames needs the option 'type', e.g. '{ type: \"region\" }', "
                   "which JavaScript would throw a TypeError without");
  }
  // Digits: their ranges, and a minimum above its maximum.
  static const struct {
    const char *name;
    int min, max;
  } ranges[] = {{"minimumIntegerDigits", 1, 21},     {"minimumFractionDigits", 0, 100},
                {"maximumFractionDigits", 0, 100},   {"minimumSignificantDigits", 1, 21},
                {"maximumSignificantDigits", 1, 21}, {"roundingIncrement", 1, 5000}};
  for (const auto &range : ranges) {
    if (std::optional<double> n = numberOf(range.name);
        n && !(*n >= range.min && *n <= range.max)) {
      report(range.name, std::string(range.name) + " is from " + std::to_string(range.min) +
                             " to " + std::to_string(range.max) +
                             ", and JavaScript would throw a RangeError");
    }
  }
  for (const char *pair : {"FractionDigits", "SignificantDigits"}) {
    std::string min = std::string("minimum") + pair, max = std::string("maximum") + pair;
    std::optional<double> low = numberOf(min), high = numberOf(max);
    if (low && high && std::floor(*low) > std::floor(*high)) {
      report(max, min + " is above " + max + ", and JavaScript would throw a RangeError");
    }
  }
  if (std::optional<double> n = numberOf("roundingIncrement")) {
    static const int increments[] = {1,   2,   5,   10,   20,   25,   50,  100,
                                     200, 250, 500, 1000, 2000, 2500, 5000};
    if (!llvm::is_contained(increments, *n)) {
      report("roundingIncrement", "roundingIncrement is one of 1, 2, 5, 10, 20, 25, 50, 100, "
                                  "200, 250, 500, 1000, 2000, 2500 and 5000, and JavaScript "
                                  "would throw a RangeError");
    }
  }

  return ok;
}

} // namespace tz
