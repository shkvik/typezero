#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

namespace {

const IntlService kSupported = {IntlKind::None, "supportedLocalesOf", 0, {kMatcher}, true};

}

std::optional<std::u16string> IntlOptionsChecker::literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

std::optional<double> IntlOptionsChecker::literalNumber(const Expr *e) {
  if (auto *lit = dyn_cast<IntLitExpr>(e)) {
    return double(lit->value);
  }
  if (auto *lit = dyn_cast<FloatLitExpr>(e)) {
    return lit->value;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(e); unary && unary->op == UnaryOp::Neg) {
    if (std::optional<double> value = IntlOptionsChecker::literalNumber(unary->operand)) {
      return -*value;
    }
  }

  return std::nullopt;
}

const IntlService *IntlOptionsChecker::serviceOf(IntlKind kind) {
  for (const IntlService &s : IntlCheckHelpers::services()) {
    if (s.kind == kind) {
      return &s;
    }
  }

  return nullptr;
}

bool IntlOptionsChecker::isSanctionedUnit(llvm::StringRef unit) {
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

bool IntlOptionsChecker::isWellFormedUnit(llvm::StringRef unit) {
  if (IntlOptionsChecker::isSanctionedUnit(unit)) {
    return true;
  }
  auto [numerator, denominator] = unit.split("-per-");

  return !denominator.empty() && IntlOptionsChecker::isSanctionedUnit(numerator) &&
         IntlOptionsChecker::isSanctionedUnit(denominator);
}

bool IntlOptionsChecker::isKeywordType(llvm::StringRef text) {
  llvm::SmallVector<llvm::StringRef, 4> parts;
  text.split(parts, '-');

  return llvm::all_of(parts, [](llvm::StringRef part) {
    return part.size() >= 3 && part.size() <= 8 && llvm::all_of(part, llvm::isAlnum);
  });
}

std::optional<std::string> IntlOptionsChecker::knownText(const Expr *value, const Type *type) {
  if (std::optional<std::u16string> text =
          value ? IntlOptionsChecker::literalText(value) : std::nullopt) {
    return ToUtf8::toUtf8(*text);
  }
  if (type && type->isStringLiteral()) {
    return ToUtf8::toUtf8(type->literalText());
  }

  return std::nullopt;
}

std::vector<std::string> IntlOptionsChecker::literalTexts(const Type *type, bool &open) {
  std::vector<std::string> texts;
  type = type->nonOptional();
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  for (const Type *m : members) {
    if (m->isStringLiteral()) {
      texts.push_back(ToUtf8::toUtf8(m->literalText()));
    } else if (!m->isBool() && !(m->isLiteral() && m->literalBase()->isBool())) {
      open = true;
    }
  }

  return texts;
}

const Expr *IntlOptionsChecker::unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool IntlOptionsChecker::isStringLike(const Type *type) {
  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() &&
          llvm::all_of(type->members(), [](const Type *m) { return m->isStringLiteral(); }));
}

IntlOptionsChecker::IntlOptionsChecker(SemanticState &owner)
    : state(owner) {}

IntlOptionsChecker::ResultOptionsGiven
IntlOptionsChecker::optionsGiven(const Type *type, IntlKind kind, llvm::StringRef what) {
  if (type->isUndefined() && kind == IntlKind::DisplayNames) {
    std::string message = ErrorTextIntlOptions::noType();

    return err(IntlOptionsCheckerOptionsGivenError{
        .code = IntlOptionsCheckerOptionsGivenErrorCode::NO_DISPLAY_TYPE,
        .message = message,
    });
  }
  bool object = type->isUndefined() || (type->isObject() && !type->isTuple() && !type->isClass() &&
                                        !type->isRegExp() && !type->isMatch() && !type->isDate() &&
                                        !type->isIntl() && !type->isCrypto() && !type->isBytes());
  if (!object) {
    std::string message = ErrorTextIntlOptions::notObject(what, type->name());

    return err(IntlOptionsCheckerOptionsGivenError{
        .code = IntlOptionsCheckerOptionsGivenErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

bool IntlOptionsChecker::checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind,
                                       llvm::StringRef what) {
  const IntlService *service =
      kind == IntlKind::None ? &kSupported : IntlOptionsChecker::serviceOf(kind);
  bool ok = true;
  if (args.size() > from) {
    Expr *&locale = args[from];
    locale = ExprChecker(this->state).checkExpr(locale);
    ok = IntlChecker(this->state).checkLocaleArg(locale, what);
  }
  if (args.size() <= from + 1) {
    return ok;
  }
  Expr *&options = args[from + 1];
  options = ExprChecker(this->state).checkExpr(options);
  const Type *type = options->type;
  if (type->isError()) {
    return false;
  }
  ResultOptionsGiven passed = IntlOptionsChecker::optionsGiven(type, kind, what);
  if (passed.isErr()) {
    this->state.getDiagnostics().error(options->loc(), passed.error().message);
    return false;
  }
  if (type->isUndefined()) {
    return ok;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  auto valueOf = [&](llvm::StringRef name) -> Expr * {
    if (!literal) {
      return nullptr;
    }
    for (const ObjectLitExpr::Property &prop : literal->props) {
      if (!prop.spread() && prop.name == name) {
        return prop.value;
      }
    }

    return nullptr;
  };
  auto textOf = [&](llvm::StringRef name) -> std::optional<std::string> {
    const Field *field = type->field(name);

    return field ? IntlOptionsChecker::knownText(valueOf(name), field->type->nonOptional())
                 : std::nullopt;
  };
  auto numberOf = [&](llvm::StringRef name) -> std::optional<double> {
    Expr *value = valueOf(name);

    return value ? IntlOptionsChecker::literalNumber(value) : std::nullopt;
  };
  auto report = [&](llvm::StringRef name, const std::string &message) {
    Expr *value = valueOf(name);
    this->state.getDiagnostics().error(value ? value->loc() : options->loc(), message);
    ok = false;
  };

  for (const Field &field : type->fields()) {
    const Type *value = field.type->nonOptional();
    auto found =
        llvm::find_if(service->options, [&](const IntlOption &o) { return o.name == field.name; });
    if (found == service->options.end()) {
      std::string message = ErrorTextIntlField::unknown(field.name, what, service);
      report(field.name, message);
      continue;
    }
    if (value->isUndefined()) {
      continue;
    }
    auto isBoolean = [](const Type *t) {
      return t->isBool() || (t->isLiteral() && t->literalBase()->isBool());
    };
    auto isNumber = [](const Type *t) {
      return t->isNumeric() || (t->isLiteral() && t->literalBase()->isNumeric());
    };
    bool fits = found->kind == 's'   ? IntlOptionsChecker::isStringLike(value)
                : found->kind == 'n' ? isNumber(value) || (value->isUnion() &&
                                                           llvm::all_of(value->members(), isNumber))
                : found->kind == 'b'
                    ? isBoolean(value)
                    : IntlOptionsChecker::isStringLike(value) || isBoolean(value) ||
                          (value->isUnion() && llvm::all_of(value->members(), [&](const Type *m) {
                             return IntlOptionsChecker::isStringLike(m) || isBoolean(m);
                           }));
    if (!fits) {
      std::string message = ErrorTextIntlField::kind(field.name, found->kind, field.type->name());
      report(field.name, message);
      continue;
    }
    if (std::optional<std::string> text = textOf(field.name);
        text && !found->values.empty() && !llvm::is_contained(found->values, *text) &&
        !(found->kind == 'g' && text->empty())) {
      std::string message = ErrorTextIntlField::badValue(*text, field.name, found->values);
      report(field.name, message);
      continue;
    }
    if (!textOf(field.name) && !found->values.empty() && found->kind != 'n' && found->kind != 'b') {
      bool open = false;
      std::vector<std::string> texts = IntlOptionsChecker::literalTexts(value, open);
      if (Expr *given = valueOf(field.name)) {
        if (std::optional<std::vector<std::string>> known =
                LiteralTypes(this->state).knownTexts(given)) {
          texts = *known;
          open = false;
        }
      }
      auto wrong = llvm::find_if(texts, [&](const std::string &t) {
        return !llvm::is_contained(found->values, t) && !(found->kind == 'g' && t.empty());
      });
      if (wrong != texts.end()) {
        std::string message = ErrorTextIntlField::badValue(*wrong, field.name, found->values);
        report(field.name, message);
      } else if (open) {
        std::string message = ErrorTextIntlField::open(field.name, found->values);
        report(field.name, message);
      }
      continue;
    }
  }
  if (!ok) {
    return false;
  }

  if (std::optional<std::string> code = textOf("currency");
      code && !(code->size() == 3 && llvm::all_of(*code, llvm::isAlpha))) {
    std::string message = ErrorTextIntlField::currency(*code);
    report("currency", message);
  }
  if (std::optional<std::string> unit = textOf("unit");
      unit && !IntlOptionsChecker::isWellFormedUnit(*unit)) {
    std::string message = ErrorTextIntlField::unit(*unit);
    report("unit", message);
  }
  for (const char *name : {"numberingSystem", "collation"}) {
    if (std::optional<std::string> value = textOf(name);
        value && !IntlOptionsChecker::isKeywordType(*value)) {
      std::string message = ErrorTextIntlField::keyword(*value, name);
      report(name, message);
    }
  }
  if (const Field *style = type->field("style"); style && kind == IntlKind::NumberFormat) {
    bool open = false;
    std::vector<std::string> styles = IntlOptionsChecker::literalTexts(style->type, open);
    if (std::optional<std::string> text = textOf("style")) {
      styles = {*text};
    }
    if (llvm::is_contained(styles, "currency") && !type->field("currency")) {
      std::string message = ErrorTextIntlField::styleCur();
      report("style", message);
    }
    if (llvm::is_contained(styles, "unit") && !type->field("unit")) {
      std::string message = ErrorTextIntlField::styleUnit();
      report("style", message);
    }
  }
  if (kind == IntlKind::DisplayNames && !type->field("type")) {
    std::string message = ErrorTextIntlOptions::noType();
    report("type", message);
  }
  static const struct {
    const char *name;
    int min, max;
  } ranges[] = {{"minimumIntegerDigits", 1, 21},
                {"minimumFractionDigits", 0, 100},
                {"maximumFractionDigits", 0, 100},
                {"minimumSignificantDigits", 1, 21},
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

    return value ? IntlOptionsChecker::unconverted(value) : nullptr;
  };
  for (const auto &range : ranges) {
    if (!type->field(range.name)) {
      continue;
    }
    if (std::optional<double> n = constantOf(range.name)) {
      if (!(*n >= range.min && *n <= range.max)) {
        std::string message = ErrorTextIntlField::range(range.name, range.min, range.max);
        report(range.name, message);
      }
      continue;
    }
    if (std::optional<std::pair<int64_t, int64_t>> held =
            LiteralTypes::intRange(type->field(range.name)->type->nonOptional());
        held && held->first >= range.min && held->second <= range.max) {
      continue;
    }
    const Expr *value = valueExpr(range.name);
    if (value && value->type->isInt() &&
        ByteProofs(this->state).provesFits(value, range.min, range.max)) {
      continue;
    }
    std::string message = ErrorTextIntlField::rangeOpen(range.name, range.min, range.max);
    report(range.name, message);
  }
  auto notAbove = [&](llvm::StringRef low, llvm::StringRef high) {
    std::optional<double> a = constantOf(low);
    std::optional<double> b = constantOf(high);
    if (a && b) {
      return std::floor(*a) <= std::floor(*b);
    }
    const Expr *x = valueExpr(low);
    const Expr *y = valueExpr(high);
    std::optional<Linear> lx = x ? IndexProofs(this->state).linear(x) : std::nullopt;
    std::optional<Linear> ly = y ? IndexProofs(this->state).linear(y) : std::nullopt;
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

    return this->state.getFlowState().getFacts().proves(lx->term, ly->term,
                                                        ly->offset - lx->offset + 1);
  };
  for (const char *pair : {"FractionDigits", "SignificantDigits"}) {
    std::string min = std::string("minimum") + pair;
    std::string max = std::string("maximum") + pair;
    if (!literal || !type->field(min) || !type->field(max) || notAbove(min, max)) {
      continue;
    }
    std::optional<double> low = constantOf(min);
    std::optional<double> high = constantOf(max);
    std::string message = ErrorTextIntlField::above(min, max, low && high);
    report(max, message);
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
    if (literal && known && n != 1.0 && (!n || llvm::is_contained(increments, *n))) {
      if (type->field("minimumSignificantDigits") || type->field("maximumSignificantDigits")) {
        std::string message = ErrorTextIntlField::incSig();
        report("roundingIncrement", message);
      } else if (!type->field("minimumFractionDigits") || !type->field("maximumFractionDigits") ||
                 !notAbove("minimumFractionDigits", "maximumFractionDigits") ||
                 !notAbove("maximumFractionDigits", "minimumFractionDigits")) {
        std::string message = ErrorTextIntlField::incSame();
        report("roundingIncrement", message);
      }
    }
    if (!known || (n && !llvm::is_contained(increments, *n))) {
      std::string message = ErrorTextIntlField::increments(n.has_value());
      report("roundingIncrement", message);
    }
  }

  return ok;
}

}
