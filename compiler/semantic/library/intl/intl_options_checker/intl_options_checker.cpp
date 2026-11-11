#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
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

IntlOptionsChecker::IntlOptionsChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.byteProofs;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->intlChecker = &all.intlChecker;
  this->literalTypes = &all.literalTypes;
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
  bool isNegative = false;
  const Expr *inner = e;
  auto *negation = dyn_cast<UnaryExpr>(inner);
  while (negation != nullptr && negation->op == UnaryOp::Neg) {
    isNegative = !isNegative;
    inner = negation->operand;
    negation = dyn_cast<UnaryExpr>(inner);
  }
  std::optional<double> value = IntlOptionsChecker::plainNumber(inner);
  if (!value.has_value() || !isNegative) {
    return value;
  }

  return -*value;
}

std::optional<double> IntlOptionsChecker::plainNumber(const Expr *e) {
  if (auto *lit = dyn_cast<IntLitExpr>(e)) {
    return double(lit->value);
  }
  if (auto *lit = dyn_cast<FloatLitExpr>(e)) {
    return lit->value;
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

IntlOptionsChecker::ResultOptionsGiven
IntlOptionsChecker::optionsGiven(const Type *type, IntlKind kind, llvm::StringRef what) {
  if (type->isUndefined() && kind == IntlKind::DisplayNames) {
    std::string message = ErrorTextIntlOptions::noType();

    return err(IntlOptionsCheckerOptionsGivenError{
        .code = IntlOptionsCheckerOptionsGivenErrorCode::NO_DISPLAY_TYPE,
        .message = message,
    });
  }
  bool isObject =
      type->isUndefined() || (type->isObject() && !type->isTuple() && !type->isClass() &&
                              !type->isRegExp() && !type->isMatch() && !type->isDate() &&
                              !type->isIntl() && !type->isCrypto() && !type->isBytes());
  if (!isObject) {
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
  bool isOk = true;
  if (args.size() > from) {
    Expr *&locale = args[from];
    locale = this->exprChecker->checkExpr(locale);
    isOk = this->intlChecker->checkLocaleArg(locale, what);
  }
  if (args.size() <= from + 1) {
    return isOk;
  }
  Expr *&options = args[from + 1];
  options = this->exprChecker->checkExpr(options);
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
    return isOk;
  }
  IntlOptionsRun run{
      .type = type,
      .literal = dyn_cast<ObjectLitExpr>(options),
      .options = options,
      .kind = kind,
      .what = what,
      .service = service,
      .ok = isOk,
  };
  for (const Field &field : type->fields()) {
    this->walkOptionField(run, field);
  }
  if (!run.ok) {
    return false;
  }
  this->walkOptionTexts(run);
  this->walkDigitRanges(run);
  this->walkDigitPairs(run);
  this->walkRoundingIncrement(run);

  return run.ok;
}

Expr *IntlOptionsChecker::valueOf(const IntlOptionsRun &run, llvm::StringRef name) {
  if (run.literal == nullptr) {
    return nullptr;
  }
  for (const ObjectLitExpr::Property &prop : run.literal->props) {
    if (!prop.spread() && prop.name == name) {
      return prop.value;
    }
  }

  return nullptr;
}

std::optional<std::string> IntlOptionsChecker::textOf(const IntlOptionsRun &run,
                                                      llvm::StringRef name) {
  const Field *field = run.type->field(name);
  if (field == nullptr) {
    return std::nullopt;
  }
  Expr *value = IntlOptionsChecker::valueOf(run, name);

  return IntlOptionsChecker::knownText(value, field->type->nonOptional());
}

std::optional<double> IntlOptionsChecker::constantOf(const IntlOptionsRun &run,
                                                     llvm::StringRef name) {
  Expr *value = IntlOptionsChecker::valueOf(run, name);
  std::optional<double> n =
      value != nullptr ? IntlOptionsChecker::literalNumber(value) : std::nullopt;
  if (n.has_value()) {
    return n;
  }
  const Field *field = run.type->field(name);
  const Type *t = field != nullptr ? field->type->nonOptional() : nullptr;
  if (t != nullptr && t->isLiteral() && t->literalBase()->isNumeric()) {
    return t->literalNumber();
  }

  return std::nullopt;
}

const Expr *IntlOptionsChecker::valueExpr(const IntlOptionsRun &run, llvm::StringRef name) {
  Expr *value = IntlOptionsChecker::valueOf(run, name);

  return value != nullptr ? IntlOptionsChecker::unconverted(value) : nullptr;
}

void IntlOptionsChecker::reportOption(IntlOptionsRun &run, llvm::StringRef name,
                                      const std::string &message) {
  Expr *value = IntlOptionsChecker::valueOf(run, name);
  llvm::SMLoc at = value != nullptr ? value->loc() : run.options->loc();
  this->state.getDiagnostics().error(at, message);
  run.ok = false;
}

const IntlOption *IntlOptionsChecker::findOption(const IntlService *service, llvm::StringRef name) {
  for (const IntlOption &option : service->options) {
    if (option.name == name) {
      return &option;
    }
  }

  return nullptr;
}

void IntlOptionsChecker::walkOptionField(IntlOptionsRun &run, const Field &field) {
  const Type *value = field.type->nonOptional();
  const IntlOption *found = IntlOptionsChecker::findOption(run.service, field.name);
  if (found == nullptr) {
    std::string message = ErrorTextIntlField::unknown(field.name, run.what, run.service);
    this->reportOption(run, field.name, message);
    return;
  }
  if (value->isUndefined()) {
    return;
  }
  if (!IntlOptionsChecker::optionKindFits(found->kind, value)) {
    std::string message = ErrorTextIntlField::kind(field.name, found->kind, field.type->name());
    this->reportOption(run, field.name, message);
    return;
  }
  std::optional<std::string> text = IntlOptionsChecker::textOf(run, field.name);
  bool isBadText = text.has_value() && !found->values.empty() &&
                   !llvm::is_contained(found->values, *text) &&
                   !(found->kind == 'g' && text->empty());
  if (isBadText) {
    std::string message = ErrorTextIntlField::badValue(*text, field.name, found->values);
    this->reportOption(run, field.name, message);
    return;
  }
  if (!text.has_value() && !found->values.empty() && found->kind != 'n' && found->kind != 'b') {
    this->walkOptionValues(run, field, *found, value);
  }
}

bool IntlOptionsChecker::optionKindFits(char kind, const Type *value) {
  switch (kind) {
    case 's':
      return IntlOptionsChecker::isStringLike(value);
    case 'n':
      return IntlOptionsChecker::isNumberLike(value) ||
             (value->isUnion() && IntlOptionsChecker::allNumberLike(value->members()));
    case 'b':
      return IntlOptionsChecker::isBooleanLike(value);
  }

  return IntlOptionsChecker::isStringLike(value) || IntlOptionsChecker::isBooleanLike(value) ||
         (value->isUnion() && IntlOptionsChecker::allTextOrBoolean(value->members()));
}

bool IntlOptionsChecker::isBooleanLike(const Type *t) {
  return t->isBool() || (t->isLiteral() && t->literalBase()->isBool());
}

bool IntlOptionsChecker::isNumberLike(const Type *t) {
  return t->isNumeric() || (t->isLiteral() && t->literalBase()->isNumeric());
}

bool IntlOptionsChecker::allNumberLike(llvm::ArrayRef<const Type *> members) {
  for (const Type *m : members) {
    if (!IntlOptionsChecker::isNumberLike(m)) {
      return false;
    }
  }

  return true;
}

bool IntlOptionsChecker::allTextOrBoolean(llvm::ArrayRef<const Type *> members) {
  for (const Type *m : members) {
    if (!IntlOptionsChecker::isStringLike(m) && !IntlOptionsChecker::isBooleanLike(m)) {
      return false;
    }
  }

  return true;
}

void IntlOptionsChecker::walkOptionValues(IntlOptionsRun &run, const Field &field,
                                          const IntlOption &found, const Type *value) {
  bool isOpen = false;
  std::vector<std::string> texts = IntlOptionsChecker::literalTexts(value, isOpen);
  Expr *given = IntlOptionsChecker::valueOf(run, field.name);
  std::optional<std::vector<std::string>> known =
      given != nullptr ? this->literalTypes->knownTexts(given) : std::nullopt;
  if (known.has_value()) {
    texts = *known;
    isOpen = false;
  }
  const std::string *wrong = IntlOptionsChecker::wrongText(texts, found);
  if (wrong != nullptr) {
    std::string message = ErrorTextIntlField::badValue(*wrong, field.name, found.values);
    this->reportOption(run, field.name, message);
  } else if (isOpen) {
    std::string message = ErrorTextIntlField::open(field.name, found.values);
    this->reportOption(run, field.name, message);
  }
}

const std::string *IntlOptionsChecker::wrongText(const std::vector<std::string> &texts,
                                                 const IntlOption &found) {
  for (const std::string &t : texts) {
    if (!llvm::is_contained(found.values, t) && !(found.kind == 'g' && t.empty())) {
      return &t;
    }
  }

  return nullptr;
}

void IntlOptionsChecker::walkOptionTexts(IntlOptionsRun &run) {
  std::optional<std::string> code = IntlOptionsChecker::textOf(run, "currency");
  if (code.has_value() && !(code->size() == 3 && llvm::all_of(*code, llvm::isAlpha))) {
    std::string message = ErrorTextIntlField::currency(*code);
    this->reportOption(run, "currency", message);
  }
  std::optional<std::string> unit = IntlOptionsChecker::textOf(run, "unit");
  if (unit.has_value() && !IntlOptionsChecker::isWellFormedUnit(*unit)) {
    std::string message = ErrorTextIntlField::unit(*unit);
    this->reportOption(run, "unit", message);
  }
  for (const char *name : {"numberingSystem", "collation"}) {
    std::optional<std::string> value = IntlOptionsChecker::textOf(run, name);
    if (value.has_value() && !IntlOptionsChecker::isKeywordType(*value)) {
      std::string message = ErrorTextIntlField::keyword(*value, name);
      this->reportOption(run, name, message);
    }
  }
  this->walkStyleOption(run);
  if (run.kind == IntlKind::DisplayNames && run.type->field("type") == nullptr) {
    std::string message = ErrorTextIntlOptions::noType();
    this->reportOption(run, "type", message);
  }
}

void IntlOptionsChecker::walkStyleOption(IntlOptionsRun &run) {
  const Field *style = run.type->field("style");
  if (style == nullptr || run.kind != IntlKind::NumberFormat) {
    return;
  }
  bool isOpen = false;
  std::vector<std::string> styles = IntlOptionsChecker::literalTexts(style->type, isOpen);
  std::optional<std::string> text = IntlOptionsChecker::textOf(run, "style");
  if (text.has_value()) {
    styles = {*text};
  }
  if (llvm::is_contained(styles, "currency") && run.type->field("currency") == nullptr) {
    std::string message = ErrorTextIntlField::styleCur();
    this->reportOption(run, "style", message);
  }
  if (llvm::is_contained(styles, "unit") && run.type->field("unit") == nullptr) {
    std::string message = ErrorTextIntlField::styleUnit();
    this->reportOption(run, "style", message);
  }
}

void IntlOptionsChecker::walkDigitRanges(IntlOptionsRun &run) {
  static const struct {
    const char *name;
    int min, max;
  } ranges[] = {{"minimumIntegerDigits", 1, 21},
                {"minimumFractionDigits", 0, 100},
                {"maximumFractionDigits", 0, 100},
                {"minimumSignificantDigits", 1, 21},
                {"maximumSignificantDigits", 1, 21}};
  for (const auto &range : ranges) {
    this->walkDigitRange(run, range.name, range.min, range.max);
  }
}

void IntlOptionsChecker::walkDigitRange(IntlOptionsRun &run, const char *name, int min, int max) {
  const Field *field = run.type->field(name);
  if (field == nullptr) {
    return;
  }
  std::optional<double> n = IntlOptionsChecker::constantOf(run, name);
  if (n.has_value()) {
    if (!(*n >= min && *n <= max)) {
      std::string message = ErrorTextIntlField::range(name, min, max);
      this->reportOption(run, name, message);
    }
    return;
  }
  std::optional<std::pair<int64_t, int64_t>> held =
      LiteralTypes::intRange(field->type->nonOptional());
  if (held.has_value() && held->first >= min && held->second <= max) {
    return;
  }
  const Expr *value = IntlOptionsChecker::valueExpr(run, name);
  if (value != nullptr && value->type->isInt() && this->byteProofs->provesFits(value, min, max)) {
    return;
  }
  std::string message = ErrorTextIntlField::rangeOpen(name, min, max);
  this->reportOption(run, name, message);
}

bool IntlOptionsChecker::isNotAbove(const IntlOptionsRun &run, llvm::StringRef low,
                                    llvm::StringRef high) {
  std::optional<double> a = IntlOptionsChecker::constantOf(run, low);
  std::optional<double> b = IntlOptionsChecker::constantOf(run, high);
  if (a.has_value() && b.has_value()) {
    return std::floor(*a) <= std::floor(*b);
  }
  const Expr *x = IntlOptionsChecker::valueExpr(run, low);
  const Expr *y = IntlOptionsChecker::valueExpr(run, high);
  std::optional<Linear> lx = x != nullptr ? this->indexProofs->linear(x) : std::nullopt;
  std::optional<Linear> ly = y != nullptr ? this->indexProofs->linear(y) : std::nullopt;
  if (!lx.has_value() && a.has_value() && *a == std::floor(*a)) {
    lx = Linear{Term::zero(), int64_t(*a)};
  }
  if (!ly.has_value() && b.has_value() && *b == std::floor(*b)) {
    ly = Linear{Term::zero(), int64_t(*b)};
  }
  if (!lx.has_value() || !ly.has_value()) {
    return false;
  }
  if (lx->term == ly->term) {
    return lx->offset <= ly->offset;
  }

  return this->state.getFlowState().getFacts().proves(lx->term, ly->term,
                                                      ly->offset - lx->offset + 1);
}

void IntlOptionsChecker::walkDigitPairs(IntlOptionsRun &run) {
  for (const char *pair : {"FractionDigits", "SignificantDigits"}) {
    std::string min = std::string("minimum") + pair;
    std::string max = std::string("maximum") + pair;
    bool isSkipped = run.literal == nullptr || run.type->field(min) == nullptr ||
                     run.type->field(max) == nullptr || this->isNotAbove(run, min, max);
    if (isSkipped) {
      continue;
    }
    std::optional<double> low = IntlOptionsChecker::constantOf(run, min);
    std::optional<double> high = IntlOptionsChecker::constantOf(run, max);
    std::string message = ErrorTextIntlField::above(min, max, low.has_value() && high.has_value());
    this->reportOption(run, max, message);
  }
}

bool IntlOptionsChecker::isIncrement(double value) {
  static const int increments[] = {1,   2,   5,   10,   20,   25,   50,  100,
                                   200, 250, 500, 1000, 2000, 2500, 5000};

  return llvm::is_contained(increments, value);
}

bool IntlOptionsChecker::allIncrements(const Type *t) {
  if (!t->isUnion()) {
    return false;
  }
  for (const Type *m : t->members()) {
    if (!m->isLiteral() || !IntlOptionsChecker::isIncrement(m->literalNumber())) {
      return false;
    }
  }

  return true;
}

void IntlOptionsChecker::walkRoundingIncrement(IntlOptionsRun &run) {
  const Field *increment = run.type->field("roundingIncrement");
  if (increment == nullptr) {
    return;
  }
  std::optional<double> n = IntlOptionsChecker::constantOf(run, "roundingIncrement");
  bool isKnown = n.has_value() || IntlOptionsChecker::allIncrements(increment->type->nonOptional());
  bool isListed = !n.has_value() || IntlOptionsChecker::isIncrement(*n);
  if (run.literal != nullptr && isKnown && n != 1.0 && isListed) {
    this->walkIncrementDigits(run);
  }
  if (!isKnown || !isListed) {
    std::string message = ErrorTextIntlField::increments(n.has_value());
    this->reportOption(run, "roundingIncrement", message);
  }
}

void IntlOptionsChecker::walkIncrementDigits(IntlOptionsRun &run) {
  bool isSignificant = run.type->field("minimumSignificantDigits") != nullptr ||
                       run.type->field("maximumSignificantDigits") != nullptr;
  if (isSignificant) {
    std::string message = ErrorTextIntlField::incSig();
    this->reportOption(run, "roundingIncrement", message);
    return;
  }
  bool isSameDigits = run.type->field("minimumFractionDigits") != nullptr &&
                      run.type->field("maximumFractionDigits") != nullptr &&
                      this->isNotAbove(run, "minimumFractionDigits", "maximumFractionDigits") &&
                      this->isNotAbove(run, "maximumFractionDigits", "minimumFractionDigits");
  if (!isSameDigits) {
    std::string message = ErrorTextIntlField::incSame();
    this->reportOption(run, "roundingIncrement", message);
  }
}

}
