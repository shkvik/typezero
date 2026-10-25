// Intl beyond dates (docs/intl.md): Intl.NumberFormat, Collator,
// PluralRules, RelativeTimeFormat, ListFormat, DisplayNames and Segmenter,
// their methods, and the values `nf.format` and `collator.compare` (bound to
// their objects, as in JavaScript); toLocaleString of numbers and decimals,
// localeCompare and normalize of strings; Intl.getCanonicalLocales and
// supportedLocalesOf. Their objects are objects of the runtime
// (runtime/intl.h), and the calls are built-ins (Builtin::IntlNew...).
//
// The options are checked as for dates (sema_date.cpp): an object whose
// fields are options of the kind, each of its type. What JavaScript rejects
// with an exception when the program runs and is written in the code is an
// error here: a locale that is no language tag, a value that is not one of
// the option, a currency or a unit that is not one, style "currency"
// without a currency, digits out of range.

#include "sema/sema.h"

#include "../../runtime/intl.h"

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

// A number written in the code: a literal, or one negated.
std::optional<double> literalNumber(const Expr *e) {
  if (auto *lit = dyn_cast<IntLitExpr>(e))
    return double(lit->value);
  if (auto *lit = dyn_cast<FloatLitExpr>(e))
    return lit->value;
  if (auto *unary = dyn_cast<UnaryExpr>(e); unary && unary->op == UnaryOp::Neg)
    if (std::optional<double> value = literalNumber(unary->operand))
      return -*value;
  return std::nullopt;
}

// An option: its type ('s' a string, 'n' a number, 'b' a boolean, 'g' a
// string or a boolean), and the values a string may have (none: any).
struct IntlOption {
  llvm::StringLiteral name;
  char kind;
  std::vector<llvm::StringLiteral> values;
};

const IntlOption kMatcher = {"localeMatcher", 's', {"lookup", "best fit"}};
const IntlOption kNumbering = {"numberingSystem", 's', {}};

// The digit options of NumberFormat and PluralRules.
const IntlOption kDigits[] = {
    {"minimumIntegerDigits", 'n', {}},
    {"minimumFractionDigits", 'n', {}},
    {"maximumFractionDigits", 'n', {}},
    {"minimumSignificantDigits", 'n', {}},
    {"maximumSignificantDigits", 'n', {}},
    {"roundingIncrement", 'n', {}},
    {"roundingMode",
     's',
     {"ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc",
      "halfEven"}},
    {"roundingPriority", 's', {"auto", "morePrecision", "lessPrecision"}},
    {"trailingZeroDisplay", 's', {"auto", "stripIfInteger"}},
};

std::vector<IntlOption> numberOptions() {
  std::vector<IntlOption> options = {
      kMatcher,
      kNumbering,
      {"style", 's', {"decimal", "percent", "currency", "unit"}},
      {"currency", 's', {}},
      {"currencyDisplay", 's', {"code", "symbol", "narrowSymbol", "name"}},
      {"currencySign", 's', {"standard", "accounting"}},
      {"unit", 's', {}},
      {"unitDisplay", 's', {"short", "narrow", "long"}},
      {"useGrouping", 'g', {"min2", "auto", "always", "true", "false"}},
      {"notation", 's', {"standard", "scientific", "engineering", "compact"}},
      {"compactDisplay", 's', {"short", "long"}},
      {"signDisplay", 's', {"auto", "never", "always", "exceptZero", "negative"}},
  };
  options.insert(options.end(), std::begin(kDigits), std::end(kDigits));
  return options;
}

// A kind of object of Intl: what JavaScript calls it, what its options are,
// and whether it can be made without `new`.
struct IntlService {
  IntlKind kind;
  llvm::StringLiteral name;
  int runtimeKind; // TZ_INTL_*
  std::vector<IntlOption> options;
  bool callable;
};

const std::vector<IntlService> &services() {
  static const std::vector<IntlService> table = [] {
    std::vector<IntlOption> plural = {kMatcher, {"type", 's', {"cardinal", "ordinal"}}};
    plural.insert(plural.end(), std::begin(kDigits), std::end(kDigits));
    const std::vector<llvm::StringLiteral> widths = {"long", "short", "narrow"};
    return std::vector<IntlService>{
        {IntlKind::NumberFormat, "NumberFormat", TZ_INTL_NUMBER_FORMAT, numberOptions(), true},
        {IntlKind::Collator,
         "Collator",
         TZ_INTL_COLLATOR,
         {kMatcher,
          {"usage", 's', {"sort", "search"}},
          {"sensitivity", 's', {"base", "accent", "case", "variant"}},
          {"ignorePunctuation", 'b', {}},
          {"collation", 's', {}},
          {"numeric", 'b', {}},
          {"caseFirst", 's', {"upper", "lower", "false"}}},
         true},
        {IntlKind::PluralRules, "PluralRules", TZ_INTL_PLURAL_RULES, plural, false},
        {IntlKind::RelativeTimeFormat,
         "RelativeTimeFormat",
         TZ_INTL_RELATIVE_TIME_FORMAT,
         {kMatcher, kNumbering, {"style", 's', widths}, {"numeric", 's', {"always", "auto"}}},
         false},
        {IntlKind::ListFormat,
         "ListFormat",
         TZ_INTL_LIST_FORMAT,
         {kMatcher, {"type", 's', {"conjunction", "disjunction", "unit"}}, {"style", 's', widths}},
         false},
        {IntlKind::DisplayNames,
         "DisplayNames",
         TZ_INTL_DISPLAY_NAMES,
         {kMatcher,
          {"style", 's', widths},
          {"type", 's', {"language", "region", "script", "currency", "calendar", "dateTimeField"}},
          {"fallback", 's', {"code", "none"}},
          {"languageDisplay", 's', {"dialect", "standard"}}},
         false},
        {IntlKind::Segmenter,
         "Segmenter",
         TZ_INTL_SEGMENTER,
         {kMatcher, {"granularity", 's', {"grapheme", "word", "sentence"}}},
         false},
    };
  }();
  return table;
}

const IntlService *serviceNamed(llvm::StringRef name) {
  for (const IntlService &s : services())
    if (s.name == name)
      return &s;
  return nullptr;
}

const IntlService *serviceOf(IntlKind kind) {
  for (const IntlService &s : services())
    if (s.kind == kind)
      return &s;
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
  if (isSanctionedUnit(unit))
    return true;
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
  for (size_t i = 0; i < values.size(); ++i)
    out += (i == 0 ? "'" : i + 1 == values.size() ? " or '" : ", '") + values[i].str() + "'";
  return out;
}

// What the string of a field holds, if the code says: a literal, or a
// string literal type.
std::optional<std::string> knownText(const Expr *value, const Type *type) {
  if (std::optional<std::u16string> text = value ? literalText(value) : std::nullopt)
    return utf8(*text);
  if (type && type->isStringLiteral())
    return utf8(type->literalText());
  return std::nullopt;
}

bool isStringLike(const Type *type) {
  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() &&
          llvm::all_of(type->members(), [](const Type *m) { return m->isStringLiteral(); }));
}

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

// The fields of the parts and of the data the runtime makes.
const Type *relativePartType() {
  return Type::object({{"type", Type::string(), false},
                       {"value", Type::string(), false},
                       {"unit", Type::optional(Type::string()), false}},
                      "Intl.RelativeTimeFormatPart");
}

} // namespace

const Type *Sema::segmentDataType() {
  return Type::object({{"segment", Type::string(), false},
                       {"index", Type::intTy(), false},
                       {"input", Type::string(), false},
                       {"isWordLike", Type::optional(Type::boolTy()), false}},
                      "Intl.SegmentData");
}

// What resolvedOptions() gives, with the fields in the order node prints
// them; a field that is not always there is `T | undefined`.
const Type *Sema::resolvedIntlOptionsType(IntlKind kind) {
  const Type *text = Type::string(), *maybe = Type::optional(Type::string());
  const Type *integer = Type::intTy(), *maybeInt = Type::optional(Type::intTy());
  const Type *flag = Type::boolTy();
  std::vector<Field> digits = {{"minimumIntegerDigits", integer, false},
                               {"minimumFractionDigits", maybeInt, false},
                               {"maximumFractionDigits", maybeInt, false},
                               {"minimumSignificantDigits", maybeInt, false},
                               {"maximumSignificantDigits", maybeInt, false}};
  std::vector<Field> rounding = {{"roundingIncrement", integer, false},
                                 {"roundingMode", text, false},
                                 {"roundingPriority", text, false},
                                 {"trailingZeroDisplay", text, false}};
  std::vector<Field> fields = {{"locale", text, false}};
  const char *name = "";
  switch (kind) {
  case IntlKind::NumberFormat:
    name = "Intl.ResolvedNumberFormatOptions";
    fields.insert(fields.end(), {{"numberingSystem", text, false},
                                 {"style", text, false},
                                 {"currency", maybe, false},
                                 {"currencyDisplay", maybe, false},
                                 {"currencySign", maybe, false},
                                 {"unit", maybe, false},
                                 {"unitDisplay", maybe, false}});
    fields.insert(fields.end(), digits.begin(), digits.end());
    fields.insert(fields.end(),
                  {{"useGrouping", Type::unionOf({Type::string(), Type::boolTy()}), false},
                   {"notation", text, false},
                   {"compactDisplay", maybe, false},
                   {"signDisplay", text, false}});
    fields.insert(fields.end(), rounding.begin(), rounding.end());
    break;
  case IntlKind::Collator:
    name = "Intl.ResolvedCollatorOptions";
    fields.insert(fields.end(), {{"usage", text, false},
                                 {"sensitivity", text, false},
                                 {"ignorePunctuation", flag, false},
                                 {"collation", text, false},
                                 {"numeric", flag, false},
                                 {"caseFirst", text, false}});
    break;
  case IntlKind::PluralRules:
    name = "Intl.ResolvedPluralRulesOptions";
    fields.push_back({"type", text, false});
    fields.insert(fields.end(), digits.begin(), digits.end());
    fields.push_back({"pluralCategories", Type::array(Type::string()), false});
    fields.insert(fields.end(), rounding.begin(), rounding.end());
    break;
  case IntlKind::RelativeTimeFormat:
    name = "Intl.ResolvedRelativeTimeFormatOptions";
    fields.insert(
        fields.end(),
        {{"style", text, false}, {"numeric", text, false}, {"numberingSystem", text, false}});
    break;
  case IntlKind::ListFormat:
    name = "Intl.ResolvedListFormatOptions";
    fields.insert(fields.end(), {{"type", text, false}, {"style", text, false}});
    break;
  case IntlKind::DisplayNames:
    name = "Intl.ResolvedDisplayNamesOptions";
    fields.insert(fields.end(), {{"style", text, false},
                                 {"type", text, false},
                                 {"fallback", text, false},
                                 {"languageDisplay", maybe, false}});
    break;
  default:
    name = "Intl.ResolvedSegmenterOptions";
    fields.push_back({"granularity", text, false});
    break;
  }
  return Type::object(std::move(fields), name);
}

const Type *Sema::intlTypeNamed(llvm::StringRef name) {
  if (name == "DateTimeFormat")
    return Type::dateFormat();
  if (const IntlService *service = serviceNamed(name))
    return Type::intl(service->kind);
  if (name == "Segments")
    return Type::intl(IntlKind::Segments);
  if (name == "SegmentData")
    return segmentDataType();
  if (name == "DateTimeFormatPart" || name == "NumberFormatPart")
    return dateFormatPartType();
  if (name == "RelativeTimeFormatPart")
    return relativePartType();
  if (name == "ResolvedDateTimeFormatOptions")
    return resolvedDateOptionsType();
  if (name == "DateTimeFormatOptions")
    return dateOptionsType();
  // Names of kinds of strings.
  if (name == "LDMLPluralRule" || name == "RelativeTimeFormatUnit" ||
      name == "UnicodeBCP47LocaleIdentifier" || name == "BCP47LanguageTag")
    return Type::string();
  for (const IntlService &service : services()) {
    if (name == "Resolved" + service.name.str() + "Options")
      return resolvedIntlOptionsType(service.kind);
    if (name != service.name.str() + "Options")
      continue;
    // The options as a type: every one `T | undefined`.
    std::vector<Field> fields;
    for (const IntlOption &option : service.options) {
      const Type *type = option.kind == 's'   ? Type::string()
                         : option.kind == 'n' ? Type::doubleTy()
                         : option.kind == 'b' ? Type::boolTy()
                                              : Type::unionOf({Type::string(), Type::boolTy()});
      fields.push_back({option.name.str(), Type::optional(type), false});
    }
    return Type::object(std::move(fields), "Intl." + service.name.str() + "Options");
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// The locale and the options
//===----------------------------------------------------------------------===//

// The locale and the options at args[from] and args[from + 1], both
// optional. The locale is a string or undefined; the options an object
// whose fields are options of the service, each of its type or that type or
// undefined. What JavaScript rejects with an exception and is written in the
// code is an error. False after an error.
bool Sema::checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind,
                         llvm::StringRef what) {
  const IntlService *service = kind == IntlKind::None ? &kSupported : serviceOf(kind);
  bool ok = true;
  if (args.size() > from) {
    Expr *&locale = args[from];
    std::optional<std::u16string> tag = literalText(locale);
    locale = checkExpr(locale);
    const Type *type = locale->type;
    if (type->isError()) {
      ok = false;
    } else if (!type->isUndefined() && type != Type::string() &&
               type != Type::optional(Type::string())) {
      std::string message = what.str() +
                            " takes the language as a string, e.g. \"ru-RU\", found '" +
                            type->name().str() + "'";
      if (type->isArray())
        message += "; pass one language";
      diags_.error(locale->loc(), message);
      ok = false;
    } else if (tag && !isLanguageTag(utf8(*tag))) {
      diags_.error(locale->loc(), "'" + utf8(*tag) +
                                      "' is not a language tag, and JavaScript would throw a "
                                      "RangeError; write it as BCP 47 does, e.g. 'en-US' or 'ru'");
      ok = false;
    }
  }
  if (args.size() <= from + 1)
    return ok;
  Expr *&options = args[from + 1];
  options = checkExpr(options);
  const Type *type = options->type;
  if (type->isError())
    return false;
  if (type->isUndefined()) {
    if (kind == IntlKind::DisplayNames) {
      diags_.error(options->loc(), "Intl.DisplayNames needs the option 'type', e.g. '{ type: "
                                   "\"region\" }', which JavaScript would throw a TypeError "
                                   "without");
      return false;
    }
    return ok;
  }
  if (!type->isObject() || type->isTuple() || type->isClass() || type->isRegExp() ||
      type->isMatch() || type->isDate() || type->isIntl() || type->isCrypto() ||
      type->isBytes()) {
    diags_.error(options->loc(), what.str() +
                                     " takes its options as an object, e.g. '{ style: \"percent\" "
                                     "}', found '" +
                                     type->name().str() + "'");
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  auto valueOf = [&](llvm::StringRef name) -> Expr * {
    if (!literal)
      return nullptr;
    for (const ObjectLitExpr::Property &prop : literal->props)
      if (!prop.spread && prop.name == name)
        return prop.value;
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
    diags_.error(value ? value->loc() : options->loc(), message);
    ok = false;
  };

  std::string names;
  for (size_t i = 0; i < service->options.size(); ++i)
    names += (i == 0                             ? ""
              : i + 1 == service->options.size() ? " and "
                                                 : ", ") +
             service->options[i].name.str();
  for (const Field &field : type->fields()) {
    const Type *value = field.type->nonOptional();
    auto found =
        llvm::find_if(service->options, [&](const IntlOption &o) { return o.name == field.name; });
    if (found == service->options.end()) {
      report(field.name, "'" + field.name + "' is not an option of " + what.str() +
                             "; the options are " + names);
      continue;
    }
    if (value->isUndefined()) // as if not given
      continue;
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
  if (!ok)
    return false;

  // What JavaScript checks of the values themselves.
  if (std::optional<std::string> code = textOf("currency");
      code && !(code->size() == 3 && llvm::all_of(*code, llvm::isAlpha)))
    report("currency", "'" + *code +
                           "' is not a currency code, and JavaScript would throw a RangeError; "
                           "a code is three letters, e.g. 'RUB' or 'USD'");
  if (std::optional<std::string> unit = textOf("unit"); unit && !isWellFormedUnit(*unit))
    report("unit", "'" + *unit +
                       "' is not a unit of Intl, and JavaScript would throw a RangeError; the "
                       "units are those like 'kilometer', 'liter', 'percent' or 'megabyte', and "
                       "two joined by '-per-', e.g. 'kilometer-per-hour'");
  for (const char *name : {"numberingSystem", "collation"})
    if (std::optional<std::string> value = textOf(name); value && !isKeywordType(*value))
      report(name, "'" + *value + "' is not a " + name +
                       ", and JavaScript would throw a RangeError; it is 3 to 8 letters or "
                       "digits, e.g. '" +
                       (llvm::StringRef(name) == "collation" ? "phonebk" : "latn") + "'");
  if (std::optional<std::string> style = textOf("style"); style && kind == IntlKind::NumberFormat) {
    if (*style == "currency" && !type->field("currency"))
      report("style", "style 'currency' needs the option 'currency', e.g. '{ style: "
                      "\"currency\", currency: \"RUB\" }', which JavaScript would throw a "
                      "TypeError without");
    if (*style == "unit" && !type->field("unit"))
      report("style", "style 'unit' needs the option 'unit', e.g. '{ style: \"unit\", unit: "
                      "\"kilometer\" }', which JavaScript would throw a TypeError without");
  }
  if (kind == IntlKind::DisplayNames && !type->field("type"))
    report("type", "Intl.DisplayNames needs the option 'type', e.g. '{ type: \"region\" }', "
                   "which JavaScript would throw a TypeError without");
  // Digits: their ranges, and a minimum above its maximum.
  static const struct {
    const char *name;
    int min, max;
  } ranges[] = {{"minimumIntegerDigits", 1, 21},     {"minimumFractionDigits", 0, 100},
                {"maximumFractionDigits", 0, 100},   {"minimumSignificantDigits", 1, 21},
                {"maximumSignificantDigits", 1, 21}, {"roundingIncrement", 1, 5000}};
  for (const auto &range : ranges)
    if (std::optional<double> n = numberOf(range.name); n && !(*n >= range.min && *n <= range.max))
      report(range.name, std::string(range.name) + " is from " + std::to_string(range.min) +
                             " to " + std::to_string(range.max) +
                             ", and JavaScript would throw a RangeError");
  for (const char *pair : {"FractionDigits", "SignificantDigits"}) {
    std::string min = std::string("minimum") + pair, max = std::string("maximum") + pair;
    std::optional<double> low = numberOf(min), high = numberOf(max);
    if (low && high && std::floor(*low) > std::floor(*high))
      report(max, min + " is above " + max + ", and JavaScript would throw a RangeError");
  }
  if (std::optional<double> n = numberOf("roundingIncrement")) {
    static const int increments[] = {1,   2,   5,   10,   20,   25,   50,  100,
                                     200, 250, 500, 1000, 2000, 2500, 5000};
    if (!llvm::is_contained(increments, *n))
      report("roundingIncrement", "roundingIncrement is one of 1, 2, 5, 10, 20, 25, 50, 100, "
                                  "200, 250, 500, 1000, 2000, 2500 and 5000, and JavaScript "
                                  "would throw a RangeError");
  }
  return ok;
}

//===----------------------------------------------------------------------===//
// Making the objects
//===----------------------------------------------------------------------===//

// The kind of the runtime (TZ_INTL_*) as the first argument of a built-in.
static Expr *kindArg(AstContext &ctx, llvm::SMLoc loc, int kind) {
  auto *lit = ctx.make<IntLitExpr>(loc, uint64_t(kind));
  lit->type = Type::intTy();
  return lit;
}

Expr *Sema::checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                         bool withNew) {
  auto failed = [&] {
    for (Expr *&arg : args)
      arg = checkExpr(arg);
    auto *error = ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  };
  const IntlService *service = serviceNamed(name);
  if (!service) {
    diags_.error(loc, "'Intl." + name +
                          "' is not supported yet; Intl has DateTimeFormat, NumberFormat, "
                          "Collator, PluralRules, RelativeTimeFormat, ListFormat, DisplayNames "
                          "and Segmenter");
    return failed();
  }
  if (!withNew && !service->callable) {
    diags_.error(loc, "'Intl." + name + "' is made with 'new': 'new Intl." + name +
                          "(...)', and JavaScript would throw a TypeError without it");
    return failed();
  }
  if (args.size() > 2) {
    diags_.error(loc,
                 "expected " + argumentCount(0, 2) + ", but got " + std::to_string(args.size()));
    return failed();
  }
  if (service->kind == IntlKind::DisplayNames && args.size() < 2) {
    diags_.error(loc, "Intl.DisplayNames needs the option 'type', e.g. 'new "
                      "Intl.DisplayNames(\"ru\", { type: \"region\" })', which JavaScript "
                      "would throw a TypeError without");
    return failed();
  }
  if (!checkIntlArgs(args, 0, service->kind, "Intl." + name.str())) {
    auto *error = ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  std::vector<Expr *> all{kindArg(ctx_, loc, service->runtimeKind)};
  all.insert(all.end(), args.begin(), args.end());
  return builtinCall(loc, ctx_.make<NameExpr>(loc, "Intl"), Builtin::IntlNew, std::move(all),
                     Type::intl(service->kind));
}

// The locales of getCanonicalLocales and supportedLocalesOf: a string, an
// array of them, or undefined for none. Written in the code, each must be a
// language tag.
Expr *Sema::checkLocaleList(Expr *arg, llvm::StringRef what) {
  std::vector<std::optional<std::u16string>> tags;
  if (auto *array = dyn_cast<ArrayLitExpr>(arg))
    for (const ArrayLitExpr::Element &e : array->elements)
      tags.push_back(e.spread ? std::nullopt : literalText(e.expr));
  else
    tags.push_back(literalText(arg));
  arg = checkExpr(arg, Type::array(Type::string()));
  const Type *type = arg->type;
  if (type->isError())
    return arg;
  if (type != Type::string() && type != Type::array(Type::string()) && !type->isUndefined()) {
    diags_.error(arg->loc(), what.str() + " takes a language or an array of them, found '" +
                                 type->name().str() + "'");
    return arg;
  }
  for (const std::optional<std::u16string> &tag : tags)
    if (tag && !isLanguageTag(utf8(*tag)))
      diags_.error(arg->loc(), "'" + utf8(*tag) +
                                   "' is not a language tag, and JavaScript would throw a "
                                   "RangeError; write it as BCP 47 does, e.g. 'en-US' or 'ru'");
  return arg;
}

// `Intl.X(...)` without `new`, and Intl.getCanonicalLocales.
Expr *Sema::checkIntlFunction(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  if (member->name == "getCanonicalLocales") {
    if (!checkArity(call, member->loc(), 0, 1))
      return call;
    if (!call->args.empty())
      call->args[0] = checkLocaleList(call->args[0], "Intl.getCanonicalLocales");
    call->builtin = Builtin::IntlCanonicalLocales;
    call->type = Type::array(Type::string());
    return call;
  }
  if (member->name == "supportedValuesOf") {
    diags_.error(member->loc(), "'Intl.supportedValuesOf' is not supported yet");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  return checkIntlNew(member->loc(), member->name, call->args, /*withNew=*/false);
}

// `Intl.NumberFormat.supportedLocalesOf(locales, options?)` and the others.
Expr *Sema::checkIntlStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto *service = cast<MemberExpr>(member->object);
  if (rejectSpread(call))
    return call;
  std::string what = "Intl." + service->name.str() + "." + member->name.str();
  if ((service->name != "DateTimeFormat" && !serviceNamed(service->name)) ||
      member->name != "supportedLocalesOf") {
    diags_.error(member->loc(), "'" + what +
                                    "' is not supported; of the classes of Intl, only "
                                    "'supportedLocalesOf' is");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (!checkArity(call, member->loc(), 1, 2))
    return call;
  call->args[0] = checkLocaleList(call->args[0], what);
  if (call->args[0]->type->isError())
    return call;
  // The options alone (checkIntlArgs takes a locale first, here none).
  if (call->args.size() == 2) {
    std::vector<Expr *> pair{ctx_.make<UndefinedExpr>(member->loc()), call->args[1]};
    if (!checkIntlArgs(pair, 0, IntlKind::None, what))
      return call;
    call->args[1] = pair[1];
  }
  // The name of the method, for the message of a bad localeMatcher.
  call->args.insert(call->args.begin(), ctx_.make<StringLitExpr>(member->loc(), utf16(what)));
  call->args[0]->type = Type::string();
  call->builtin = Builtin::IntlSupportedLocales;
  call->type = Type::array(Type::string());
  return call;
}

//===----------------------------------------------------------------------===//
// Methods
//===----------------------------------------------------------------------===//

// A number of format, select and toLocaleString: an int, a long, a double or
// a decimal, as it is.
Expr *Sema::checkIntlNumber(Expr *arg, llvm::StringRef method) {
  arg = apparent(checkExpr(arg));
  const Type *type = arg->type;
  if (type->isNumeric() || type->isDecimal() || type->isError())
    return arg;
  std::string message =
      "'" + method.str() + "' takes a number or a decimal, found '" + type->name().str() + "'";
  if (type->isOptional() && (type->inner()->isNumeric() || type->inner()->isDecimal()))
    message += "; the value may be undefined: check it first";
  else if (type->isString())
    message += "; read the number first, e.g. 'decimal(s)', which keeps every digit";
  diags_.error(arg->loc(), message);
  return arg;
}

Expr *Sema::checkIntlMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  IntlKind kind = member->object->type->intlKind();
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  auto done = [&](Builtin builtin, const Type *type) {
    call->builtin = builtin;
    call->type = type;
    return call;
  };
  std::string service = member->object->type->name().str();
  if (name == "resolvedOptions" && kind != IntlKind::Segments) {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    return done(Builtin::IntlResolvedOptions, resolvedIntlOptionsType(kind));
  }
  switch (kind) {
  case IntlKind::NumberFormat:
    if (name == "format" || name == "formatToParts") {
      if (!checkArity(call, member->loc(), 1, 1))
        return call;
      call->args[0] = checkIntlNumber(call->args[0], name);
      return name == "format" ? done(Builtin::NfFormat, Type::string())
                              : done(Builtin::NfFormatToParts, Type::array(dateFormatPartType()));
    }
    if (name == "formatRange" || name == "formatRangeToParts") {
      diags_.error(member->loc(), "'" + name + "' is not supported yet; format both numbers");
      return loosely();
    }
    break;
  case IntlKind::Collator:
    if (name == "compare") {
      if (!checkArity(call, member->loc(), 2, 2))
        return call;
      for (Expr *&arg : call->args)
        arg = checkArg(arg, Type::string());
      return done(Builtin::CollatorCompare, Type::intTy());
    }
    break;
  case IntlKind::PluralRules:
    if (name == "select") {
      if (!checkArity(call, member->loc(), 1, 1))
        return call;
      call->args[0] = checkIntlNumber(call->args[0], name);
      return done(Builtin::PrSelect, Type::string());
    }
    if (name == "selectRange") {
      diags_.error(member->loc(), "'selectRange' is not supported yet; select each number");
      return loosely();
    }
    break;
  case IntlKind::RelativeTimeFormat:
    if (name == "format" || name == "formatToParts") {
      if (!checkArity(call, member->loc(), 2, 2))
        return call;
      call->args[0] = numericArg(call->args[0]);
      if (call->args[0]->type->isNumeric())
        call->args[0] = convert(call->args[0], Type::doubleTy());
      std::optional<std::u16string> unit = literalText(call->args[1]);
      call->args[1] = checkArg(call->args[1], Type::string());
      static const llvm::StringLiteral units[] = {
          "year", "years", "quarter", "quarters", "month",  "months",  "week",   "weeks",
          "day",  "days",  "hour",    "hours",    "minute", "minutes", "second", "seconds"};
      if (unit && !llvm::is_contained(units, utf8(*unit)))
        diags_.error(call->args[1]->loc(),
                     "'" + utf8(*unit) +
                         "' is not a unit of Intl.RelativeTimeFormat, and JavaScript would throw "
                         "a RangeError; the units are year, quarter, month, week, day, hour, "
                         "minute and second, also in the plural");
      return name == "format" ? done(Builtin::RtfFormat, Type::string())
                              : done(Builtin::RtfFormatToParts, Type::array(relativePartType()));
    }
    break;
  case IntlKind::ListFormat:
    if (name == "format" || name == "formatToParts") {
      if (!checkArity(call, member->loc(), 1, 1))
        return call;
      call->args[0] = checkArg(call->args[0], Type::array(Type::string()));
      return name == "format" ? done(Builtin::LfFormat, Type::string())
                              : done(Builtin::LfFormatToParts, Type::array(dateFormatPartType()));
    }
    break;
  case IntlKind::DisplayNames:
    if (name == "of") {
      if (!checkArity(call, member->loc(), 1, 1))
        return call;
      call->args[0] = checkArg(call->args[0], Type::string());
      return done(Builtin::DnOf, Type::optional(Type::string()));
    }
    break;
  case IntlKind::Segmenter:
    if (name == "segment") {
      if (!checkArity(call, member->loc(), 1, 1))
        return call;
      call->args[0] = checkArg(call->args[0], Type::string());
      return done(Builtin::SegSegment, Type::intl(IntlKind::Segments));
    }
    break;
  case IntlKind::Segments:
    if (name == "containing") {
      if (!checkArity(call, member->loc(), 0, 1))
        return call;
      if (!call->args.empty()) {
        call->args[0] = numericArg(call->args[0]);
        if (call->args[0]->type->isNumeric())
          call->args[0] = convert(call->args[0], Type::doubleTy());
      }
      return done(Builtin::SegmentsContaining, Type::optional(segmentDataType()));
    }
    break;
  default:
    break;
  }
  diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + service + "'");
  return loosely();
}

// `nf.format`, `collator.compare` and `dtf.format` as values: functions bound
// to their objects, as the getters of JavaScript give them; the type of what
// they take follows where they go (`nums.map(nf.format)` passes the elements).
Expr *Sema::checkIntlMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  IntlKind kind = member->object->type->intlKind();
  llvm::StringRef name = member->name;
  // The function type the place wants: what a method passes its callback,
  // or a declared one.
  const Type *wantedParam = nullptr, *wantedRet = nullptr;
  if (callback_ && !callback_->params.empty()) {
    wantedParam = callback_->params[0];
    wantedRet = callback_->ret;
  } else if (expected && expected->nonOptional()->isFunction()) {
    const Type *fn = expected->nonOptional();
    wantedParam = fn->params().empty() ? nullptr : fn->params()[0];
    wantedRet = fn->ret();
  }
  const Type *fnType = nullptr;
  if (kind == IntlKind::NumberFormat && name == "format") {
    const Type *param = Type::doubleTy();
    if (wantedParam && (wantedParam->isNumeric() || wantedParam->isDecimal()))
      param = wantedParam;
    fnType = Type::function({param}, 1, false, Type::string());
  } else if (kind == IntlKind::DateTimeFormat && name == "format") {
    const Type *param = Type::date();
    if (wantedParam && wantedParam->isNumeric())
      param = wantedParam;
    fnType = Type::function({param}, 1, false, Type::string());
  } else if (kind == IntlKind::Collator && name == "compare") {
    const Type *ret = wantedRet && wantedRet->isFloat() ? Type::doubleTy() : Type::intTy();
    fnType = Type::function({Type::string(), Type::string()}, 2, false, ret);
  }
  if (!fnType) {
    if (kind == IntlKind::DateTimeFormat)
      diags_.error(member->loc(), "property '" + name +
                                      "' of Intl.DateTimeFormat can only be called: 'format(d)', "
                                      "'formatToParts(d)' or 'resolvedOptions()'");
    else
      diags_.error(member->loc(), "property '" + name + "' of " +
                                      member->object->type->name().str() +
                                      " can only be called, '" + name +
                                      "(...)'; as values there are only 'format' of "
                                      "Intl.NumberFormat and Intl.DateTimeFormat and 'compare' "
                                      "of Intl.Collator");
    return member;
  }
  member->type = fnType;
  return builtinCall(member->loc(), member, Builtin::IntlBound, {}, fnType);
}

//===----------------------------------------------------------------------===//
// Numbers and strings
//===----------------------------------------------------------------------===//

// x.toLocaleString(locale?, options?) of a number or a decimal: as
// Intl.NumberFormat formats it.
Expr *Sema::checkNumberLocale(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (rejectSpread(call) || !checkArity(call, member->loc(), 0, 2))
    return call;
  if (!checkIntlArgs(call->args, 0, IntlKind::NumberFormat, "toLocaleString"))
    return call;
  call->builtin = Builtin::NumToLocale;
  call->type = Type::string();
  return call;
}

// a.localeCompare(b, locale?, options?) and s.normalize(form?).
Expr *Sema::checkStringLocale(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  if (member->name == "normalize") {
    if (!checkArity(call, member->loc(), 0, 1))
      return call;
    if (!call->args.empty()) {
      std::optional<std::u16string> form = literalText(call->args[0]);
      static const llvm::StringLiteral forms[] = {"NFC", "NFD", "NFKC", "NFKD"};
      call->args[0] = checkArg(call->args[0], Type::string());
      if (form && !llvm::is_contained(forms, utf8(*form)))
        diags_.error(call->args[0]->loc(),
                     "'" + utf8(*form) +
                         "' is not a normalization form, and JavaScript would throw a "
                         "RangeError; it is 'NFC', 'NFD', 'NFKC' or 'NFKD'");
    }
    call->builtin = Builtin::StrNormalize;
    call->type = Type::string();
    return call;
  }
  // localeCompare
  if (!checkArity(call, member->loc(), 1, 3))
    return call;
  call->args[0] = checkArg(call->args[0], Type::string());
  std::vector<Expr *> rest(call->args.begin() + 1, call->args.end());
  if (!checkIntlArgs(rest, 0, IntlKind::Collator, "localeCompare"))
    return call;
  std::copy(rest.begin(), rest.end(), call->args.begin() + 1);
  call->builtin = Builtin::StrLocaleCompare;
  call->type = Type::intTy();
  return call;
}

Expr *Sema::segmentsArray(Expr *segments) {
  auto *member = ctx_.make<MemberExpr>(segments->loc(), segments, "%segments");
  const Type *type = Type::array(segmentDataType());
  member->type = type;
  return builtinCall(segments->loc(), member, Builtin::SegmentsArray, {}, type);
}

} // namespace tz
