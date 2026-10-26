#include "compiler/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
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

std::optional<std::u16string> literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

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

const IntlService kSupported = {IntlKind::None, "supportedLocalesOf", 0, {kMatcher}, true};

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

std::string valueList(const std::vector<llvm::StringLiteral> &values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    out += (i == 0 ? "'" : i + 1 == values.size() ? " or '" : ", '") + values[i].str() + "'";
  }

  return out;
}

std::optional<std::string> knownText(const Expr *value, const Type *type) {
  if (std::optional<std::u16string> text = value ? literalText(value) : std::nullopt) {
    return utf8(*text);
  }
  if (type && type->isStringLiteral()) {
    return utf8(type->literalText());
  }

  return std::nullopt;
}

std::vector<std::string> literalTexts(const Type *type, bool &open) {
  std::vector<std::string> texts;
  type = type->nonOptional();
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  for (const Type *m : members) {
    if (m->isStringLiteral()) {
      texts.push_back(utf8(m->literalText()));
    } else if (!m->isBool() && !(m->isLiteral() && m->literalBase()->isBool())) {
      open = true;
    }
  }

  return texts;
}

std::string orList(llvm::ArrayRef<std::string> values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    out += (i == 0 ? "'" : i + 1 == values.size() ? " or '" : ", '") + values[i] + "'";
  }

  return out;
}

const Expr *unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool isStringLike(const Type *type) {
  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() &&
          llvm::all_of(type->members(), [](const Type *m) { return m->isStringLiteral(); }));
}

}

IntlOptionsChecker::IntlOptionsChecker(Checker &checker) : checker_(checker) {}

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
    if (value->isUndefined()) {
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
    if (std::optional<std::string> text = textOf(field.name);
        text && !found->values.empty() && !llvm::is_contained(found->values, *text) &&
        !(found->kind == 'g' && text->empty())) {
      report(field.name, "'" + *text + "' is not a value of the option '" + field.name +
                             "', and JavaScript would throw a RangeError; it is " +
                             valueList(found->values));
      continue;
    }
    if (!textOf(field.name) && !found->values.empty() && found->kind != 'n' &&
        found->kind != 'b') {
      bool open = false;
      std::vector<std::string> texts = literalTexts(value, open);
      auto wrong = llvm::find_if(texts, [&](const std::string &t) {
        return !llvm::is_contained(found->values, t) && !(found->kind == 'g' && t.empty());
      });
      if (wrong != texts.end()) {
        report(field.name, "'" + *wrong + "' is not a value of the option '" + field.name +
                               "', and JavaScript would throw a RangeError; it is " +
                               valueList(found->values));
      } else if (open) {
        std::vector<std::string> first(found->values.begin(),
                                       found->values.begin() +
                                           std::min<size_t>(2, found->values.size()));
        std::string example;
        for (const std::string &v : first) {
          example += (example.empty() ? "" : " || ") + std::string("x == \"") + v + "\"";
        }
        report(field.name, "the option '" + field.name + "' is " + valueList(found->values) +
                               ", and a string known only when the program runs may be another, "
                               "which JavaScript would throw a RangeError for; compare it with "
                               "them first, e.g. 'if (" +
                               example + ")'");
      }
      continue;
    }
  }
  if (!ok) {
    return false;
  }

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
  if (const Field *style = type->field("style"); style && kind == IntlKind::NumberFormat) {
    bool open = false;
    std::vector<std::string> styles = literalTexts(style->type, open);
    if (std::optional<std::string> text = textOf("style")) {
      styles = {*text};
    }
    if (llvm::is_contained(styles, "currency") && !type->field("currency")) {
      report("style", "style 'currency' needs the option 'currency', e.g. '{ style: "
                      "\"currency\", currency: \"RUB\" }', which JavaScript would throw a "
                      "TypeError without");
    }
    if (llvm::is_contained(styles, "unit") && !type->field("unit")) {
      report("style", "style 'unit' needs the option 'unit', e.g. '{ style: \"unit\", unit: "
                      "\"kilometer\" }', which JavaScript would throw a TypeError without");
    }
  }
  if (kind == IntlKind::DisplayNames && !type->field("type")) {
    report("type", "Intl.DisplayNames needs the option 'type', e.g. '{ type: \"region\" }', "
                   "which JavaScript would throw a TypeError without");
  }
  static const struct {
    const char *name;
    int min, max;
  } ranges[] = {{"minimumIntegerDigits", 1, 21},     {"minimumFractionDigits", 0, 100},
                {"maximumFractionDigits", 0, 100},   {"minimumSignificantDigits", 1, 21},
                {"maximumSignificantDigits", 1, 21}};
  auto constantOf = [&](llvm::StringRef name) -> std::optional<double> {
    if (std::optional<double> n = numberOf(name)) {
      return n;
    }
    const Field *field = type->field(name);
    const Type *t = field ? field->type->nonOptional() : nullptr;
    if (t && t->isLiteral() && t->literalBase()->isNumeric()) {
      return t->literalNumber();
    }

    return std::nullopt;
  };
  auto valueExpr = [&](llvm::StringRef name) -> const Expr * {
    Expr *value = valueOf(name);

    return value ? unconverted(value) : nullptr;
  };
  for (const auto &range : ranges) {
    if (!type->field(range.name)) {
      continue;
    }
    if (std::optional<double> n = constantOf(range.name)) {
      if (!(*n >= range.min && *n <= range.max)) {
        report(range.name, std::string(range.name) + " is from " + std::to_string(range.min) +
                               " to " + std::to_string(range.max) +
                               ", and JavaScript would throw a RangeError");
      }
      continue;
    }
    const Expr *value = valueExpr(range.name);
    if (value && value->type->isInt() &&
        checker_.byteProofs().provesFits(value, range.min, range.max)) {
      continue;
    }
    std::string lo = std::to_string(range.min), hi = std::to_string(range.max);
    report(range.name, std::string(range.name) + " is from " + lo + " to " + hi +
                           ", and a number known only when the program runs may be outside, "
                           "which JavaScript would throw a RangeError for; give an int and "
                           "check it first, e.g. 'if (n >= " +
                           lo + " && n <= " + hi + ")'");
  }
  auto notAbove = [&](llvm::StringRef low, llvm::StringRef high) {
    std::optional<double> a = constantOf(low), b = constantOf(high);
    if (a && b) {
      return std::floor(*a) <= std::floor(*b);
    }
    const Expr *x = valueExpr(low), *y = valueExpr(high);
    std::optional<Linear> lx = x ? checker_.indexProofs().linear(x) : std::nullopt;
    std::optional<Linear> ly = y ? checker_.indexProofs().linear(y) : std::nullopt;
    if (!lx && a && *a == std::floor(*a)) {
      lx = Linear{Term::zero(), int64_t(*a)};
    }
    if (!ly && b && *b == std::floor(*b)) {
      ly = Linear{Term::zero(), int64_t(*b)};
    }
    if (!lx || !ly) {
      return false;
    }
    if (lx->term == ly->term) {
      return lx->offset <= ly->offset;
    }

    return checker_.flow_.facts.proves(lx->term, ly->term, ly->offset - lx->offset + 1);
  };
  for (const char *pair : {"FractionDigits", "SignificantDigits"}) {
    std::string min = std::string("minimum") + pair, max = std::string("maximum") + pair;
    if (!type->field(min) || !type->field(max) || notAbove(min, max)) {
      continue;
    }
    std::optional<double> low = constantOf(min), high = constantOf(max);
    report(max, low && high ? min + " is above " + max + ", and JavaScript would throw a RangeError"
                            : min + " may be above " + max +
                                  ", which JavaScript would throw a RangeError for; check it "
                                  "first, e.g. 'if (low <= high)'");
  }
  if (type->field("roundingIncrement")) {
    static const int increments[] = {1,   2,   5,   10,   20,   25,   50,  100,
                                     200, 250, 500, 1000, 2000, 2500, 5000};
    std::optional<double> n = constantOf("roundingIncrement");
    bool known = n.has_value();
    if (!n) {
      const Type *t = type->field("roundingIncrement")->type->nonOptional();
      known = t->isUnion() && llvm::all_of(t->members(), [&](const Type *m) {
                return m->isLiteral() && llvm::is_contained(increments, m->literalNumber());
              });
    }
    if (known && n != 1.0 && (!n || llvm::is_contained(increments, *n))) {
      if (type->field("minimumSignificantDigits") || type->field("maximumSignificantDigits")) {
        report("roundingIncrement",
               "roundingIncrement rounds to fraction digits, and with significant digits "
               "JavaScript would throw a TypeError");
      } else if (!type->field("minimumFractionDigits") || !type->field("maximumFractionDigits") ||
                 !notAbove("minimumFractionDigits", "maximumFractionDigits") ||
                 !notAbove("maximumFractionDigits", "minimumFractionDigits")) {
        report("roundingIncrement",
               "roundingIncrement needs minimumFractionDigits and maximumFractionDigits, and the "
               "same, e.g. '{ roundingIncrement: 5, minimumFractionDigits: 0, "
               "maximumFractionDigits: 0 }', or JavaScript may throw a RangeError");
      }
    }
    if (!known || (n && !llvm::is_contained(increments, *n))) {
      report("roundingIncrement",
             std::string("roundingIncrement is one of 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, "
                         "500, 1000, 2000, 2500 and 5000, and ") +
                 (n ? "JavaScript would throw a RangeError"
                    : "a number known only when the program runs may be another, which "
                      "JavaScript would throw a RangeError for; compare it with them first"));
    }
  }

  return ok;
}

}
