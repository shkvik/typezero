#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
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

std::optional<std::u16string> IntlOptionsChecker::literalText(const Expr *expr) const {
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

std::optional<double> IntlOptionsChecker::literalNumber(const Expr *expr) const {
  bool isNegative = false;
  const Expr *inner = expr;
  auto *negation = dyn_cast<UnaryExpr>(inner);
  while (negation != nullptr && negation->op == UnaryOp::Neg) {
    isNegative = !isNegative;
    inner = negation->operand;
    negation = dyn_cast<UnaryExpr>(inner);
  }
  auto value = this->plainNumber(inner);
  if (!value.has_value() || !isNegative) {
    return value;
  }

  return -*value;
}

std::optional<double> IntlOptionsChecker::plainNumber(const Expr *expr) const {
  if (auto *literal = dyn_cast<IntLitExpr>(expr)) {
    return double(literal->value);
  }
  if (auto *literal = dyn_cast<FloatLitExpr>(expr)) {
    return literal->value;
  }

  return std::nullopt;
}

const IntlService *IntlOptionsChecker::serviceOf(IntlKind kind) const {
  for (const IntlService &s : IntlCheckHelpers::services()) {
    if (s.kind == kind) {
      return &s;
    }
  }

  return nullptr;
}

bool IntlOptionsChecker::isSanctionedUnit(llvm::StringRef unit) const {
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

bool IntlOptionsChecker::isWellFormedUnit(llvm::StringRef unit) const {
  if (this->isSanctionedUnit(unit)) {
    return true;
  }
  auto [numerator, denominator] = unit.split("-per-");

  return !denominator.empty() && this->isSanctionedUnit(numerator) &&
         this->isSanctionedUnit(denominator);
}

bool IntlOptionsChecker::isKeywordType(llvm::StringRef text) const {
  auto parts = llvm::SmallVector<llvm::StringRef, 4>();
  text.split(parts, '-');

  return llvm::all_of(parts, [](llvm::StringRef part) {
    return part.size() >= 3 && part.size() <= 8 && llvm::all_of(part, llvm::isAlnum);
  });
}

std::optional<std::string> IntlOptionsChecker::knownText(const Expr *value,
                                                         const Type *type) const {
  if (std::optional<std::u16string> text =
          value != nullptr ? this->literalText(value) : std::nullopt) {
    return ToUtf8::toUtf8(*text);
  }
  if (type != nullptr && type->isStringLiteral()) {
    return ToUtf8::toUtf8(type->literalText());
  }

  return std::nullopt;
}

std::vector<std::string> IntlOptionsChecker::literalTexts(const Type *type, bool &open) const {
  auto texts = std::vector<std::string>();
  type = type->nonOptional();
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  for (const Type *member : members) {
    if (member->isStringLiteral()) {
      texts.push_back(ToUtf8::toUtf8(member->literalText()));
    } else if (!member->isBool() && !(member->isLiteral() && member->literalBase()->isBool())) {
      open = true;
    }
  }

  return texts;
}

const Expr *IntlOptionsChecker::unconverted(const Expr *expr) const {
  expr = ExprParts::withoutImplicitCasts(expr);

  return expr;
}

bool IntlOptionsChecker::isStringLike(const Type *type) const {
  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() && llvm::all_of(type->members(), [](const Type *member) {
            return member->isStringLiteral();
          }));
}

IntlOptionsChecker::ResultOptionsGiven
IntlOptionsChecker::optionsGiven(const Type *type, IntlKind kind, llvm::StringRef what) const {
  if (type->isUndefined() && kind == IntlKind::DisplayNames) {
    auto message = ErrorTextIntlOptions::noType();

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
    auto message = ErrorTextIntlOptions::notObject(what, type->name());

    return err(IntlOptionsCheckerOptionsGivenError{
        .code = IntlOptionsCheckerOptionsGivenErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

bool IntlOptionsChecker::checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind,
                                       llvm::StringRef what) {
  const IntlService *service = kind == IntlKind::None ? &kSupported : this->serviceOf(kind);
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
  auto passed = this->optionsGiven(type, kind, what);
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

Expr *IntlOptionsChecker::valueOf(const IntlOptionsRun &run, llvm::StringRef name) const {
  if (run.literal == nullptr) {
    return nullptr;
  }
  for (const ObjectLitExpr::Property &property : run.literal->props) {
    if (!property.spread() && property.name == name) {
      return property.value;
    }
  }

  return nullptr;
}

std::optional<std::string> IntlOptionsChecker::textOf(const IntlOptionsRun &run,
                                                      llvm::StringRef name) const {
  const auto *field = run.type->field(name);
  if (field == nullptr) {
    return std::nullopt;
  }
  auto *value = this->valueOf(run, name);

  return this->knownText(value, field->type->nonOptional());
}

std::optional<double> IntlOptionsChecker::constantOf(const IntlOptionsRun &run,
                                                     llvm::StringRef name) const {
  auto *value = this->valueOf(run, name);
  std::optional<double> n = value != nullptr ? this->literalNumber(value) : std::nullopt;
  if (n.has_value()) {
    return n;
  }
  const auto *field = run.type->field(name);
  const Type *type = field != nullptr ? field->type->nonOptional() : nullptr;
  if (type != nullptr && type->isLiteral() && type->literalBase()->isNumeric()) {
    return type->literalNumber();
  }

  return std::nullopt;
}

const Expr *IntlOptionsChecker::valueExpr(const IntlOptionsRun &run, llvm::StringRef name) const {
  auto *value = this->valueOf(run, name);

  return value != nullptr ? this->unconverted(value) : nullptr;
}

void IntlOptionsChecker::reportOption(IntlOptionsRun &run, llvm::StringRef name,
                                      const std::string &message) {
  auto *value = this->valueOf(run, name);
  llvm::SMLoc at = value != nullptr ? value->loc() : run.options->loc();
  this->state.getDiagnostics().error(at, message);
  run.ok = false;
}

const IntlOption *IntlOptionsChecker::findOption(const IntlService *service,
                                                 llvm::StringRef name) const {
  for (const IntlOption &option : service->options) {
    if (option.name == name) {
      return &option;
    }
  }

  return nullptr;
}

void IntlOptionsChecker::walkOptionField(IntlOptionsRun &run, const Field &field) {
  const Type *value = field.type->nonOptional();
  const auto *found = this->findOption(run.service, field.name);
  if (found == nullptr) {
    auto message = ErrorTextIntlField::unknown(field.name, run.what, run.service);
    this->reportOption(run, field.name, message);
    return;
  }
  if (value->isUndefined()) {
    return;
  }
  if (!this->optionKindFits(found->kind, value)) {
    auto message = ErrorTextIntlField::kind(field.name, found->kind, field.type->name());
    this->reportOption(run, field.name, message);
    return;
  }
  auto text = this->textOf(run, field.name);
  bool isBadText = text.has_value() && !found->values.empty() &&
                   !llvm::is_contained(found->values, *text) &&
                   !(found->kind == 'g' && text->empty());
  if (isBadText) {
    auto message = ErrorTextIntlField::badValue(*text, field.name, found->values);
    this->reportOption(run, field.name, message);
    return;
  }
  if (!text.has_value() && !found->values.empty() && found->kind != 'n' && found->kind != 'b') {
    this->walkOptionValues(run, field, *found, value);
  }
}

bool IntlOptionsChecker::optionKindFits(char kind, const Type *value) const {
  switch (kind) {
    case 's':
      return this->isStringLike(value);
    case 'n':
      return this->isNumberLike(value) ||
             (value->isUnion() && this->allNumberLike(value->members()));
    case 'b':
      return this->isBooleanLike(value);
  }

  return this->isStringLike(value) || this->isBooleanLike(value) ||
         (value->isUnion() && this->allTextOrBoolean(value->members()));
}

bool IntlOptionsChecker::isBooleanLike(const Type *type) const {
  return type->isBool() || (type->isLiteral() && type->literalBase()->isBool());
}

bool IntlOptionsChecker::isNumberLike(const Type *type) const {
  return type->isNumeric() || (type->isLiteral() && type->literalBase()->isNumeric());
}

bool IntlOptionsChecker::allNumberLike(llvm::ArrayRef<const Type *> members) const {
  for (const Type *member : members) {
    if (!this->isNumberLike(member)) {
      return false;
    }
  }

  return true;
}

bool IntlOptionsChecker::allTextOrBoolean(llvm::ArrayRef<const Type *> members) const {
  for (const Type *member : members) {
    if (!this->isStringLike(member) && !this->isBooleanLike(member)) {
      return false;
    }
  }

  return true;
}

void IntlOptionsChecker::walkOptionValues(IntlOptionsRun &run, const Field &field,
                                          const IntlOption &found, const Type *value) {
  bool isOpen = false;
  auto texts = this->literalTexts(value, isOpen);
  auto *given = this->valueOf(run, field.name);
  std::optional<std::vector<std::string>> known =
      given != nullptr ? this->literalTypes->knownTexts(given) : std::nullopt;
  if (known.has_value()) {
    texts = *known;
    isOpen = false;
  }
  const auto *wrong = this->wrongText(texts, found);
  if (wrong != nullptr) {
    auto message = ErrorTextIntlField::badValue(*wrong, field.name, found.values);
    this->reportOption(run, field.name, message);
  } else if (isOpen) {
    auto message = ErrorTextIntlField::open(field.name, found.values);
    this->reportOption(run, field.name, message);
  }
}

const std::string *IntlOptionsChecker::wrongText(const std::vector<std::string> &texts,
                                                 const IntlOption &found) const {
  for (const std::string &t : texts) {
    if (!llvm::is_contained(found.values, t) && !(found.kind == 'g' && t.empty())) {
      return &t;
    }
  }

  return nullptr;
}

void IntlOptionsChecker::walkOptionTexts(IntlOptionsRun &run) {
  auto code = this->textOf(run, "currency");
  if (code.has_value() && !(code->size() == 3 && llvm::all_of(*code, llvm::isAlpha))) {
    auto message = ErrorTextIntlField::currency(*code);
    this->reportOption(run, "currency", message);
  }
  auto unit = this->textOf(run, "unit");
  if (unit.has_value() && !this->isWellFormedUnit(*unit)) {
    auto message = ErrorTextIntlField::unit(*unit);
    this->reportOption(run, "unit", message);
  }
  for (const char *name : {"numberingSystem", "collation"}) {
    auto value = this->textOf(run, name);
    if (value.has_value() && !this->isKeywordType(*value)) {
      auto message = ErrorTextIntlField::keyword(*value, name);
      this->reportOption(run, name, message);
    }
  }
  this->walkStyleOption(run);
  if (run.kind == IntlKind::DisplayNames && run.type->field("type") == nullptr) {
    auto message = ErrorTextIntlOptions::noType();
    this->reportOption(run, "type", message);
  }
}

void IntlOptionsChecker::walkStyleOption(IntlOptionsRun &run) {
  const auto *style = run.type->field("style");
  if (style == nullptr || run.kind != IntlKind::NumberFormat) {
    return;
  }
  bool isOpen = false;
  auto styles = this->literalTexts(style->type, isOpen);
  auto text = this->textOf(run, "style");
  if (text.has_value()) {
    styles = {*text};
  }
  if (llvm::is_contained(styles, "currency") && run.type->field("currency") == nullptr) {
    auto message = ErrorTextIntlField::styleCur();
    this->reportOption(run, "style", message);
  }
  if (llvm::is_contained(styles, "unit") && run.type->field("unit") == nullptr) {
    auto message = ErrorTextIntlField::styleUnit();
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
  const auto *field = run.type->field(name);
  if (field == nullptr) {
    return;
  }
  auto n = this->constantOf(run, name);
  if (n.has_value()) {
    if (!(*n >= min && *n <= max)) {
      auto message = ErrorTextIntlField::range(name, min, max);
      this->reportOption(run, name, message);
    }
    return;
  }
  auto held = this->literalTypes->intRange(field->type->nonOptional());
  if (held.has_value() && held->first >= min && held->second <= max) {
    return;
  }
  const auto *value = this->valueExpr(run, name);
  if (value != nullptr && value->type->isInt() && this->byteProofs->provesFits(value, min, max)) {
    return;
  }
  auto message = ErrorTextIntlField::rangeOpen(name, min, max);
  this->reportOption(run, name, message);
}

bool IntlOptionsChecker::isNotAbove(const IntlOptionsRun &run, llvm::StringRef low,
                                    llvm::StringRef high) {
  auto a = this->constantOf(run, low);
  auto b = this->constantOf(run, high);
  if (a.has_value() && b.has_value()) {
    return std::floor(*a) <= std::floor(*b);
  }
  const auto *x = this->valueExpr(run, low);
  const auto *y = this->valueExpr(run, high);
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

  return this->state.getFlowState().getFacts().canProve(lx->term, ly->term,
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
    auto low = this->constantOf(run, min);
    auto high = this->constantOf(run, max);
    auto message = ErrorTextIntlField::above(min, max, low.has_value() && high.has_value());
    this->reportOption(run, max, message);
  }
}

bool IntlOptionsChecker::isIncrement(double value) const {
  static const int increments[] = {1,   2,   5,   10,   20,   25,   50,  100,
                                   200, 250, 500, 1000, 2000, 2500, 5000};

  return llvm::is_contained(increments, value);
}

bool IntlOptionsChecker::allIncrements(const Type *type) const {
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isLiteral() || !this->isIncrement(member->literalNumber())) {
      return false;
    }
  }

  return true;
}

void IntlOptionsChecker::walkRoundingIncrement(IntlOptionsRun &run) {
  const auto *increment = run.type->field("roundingIncrement");
  if (increment == nullptr) {
    return;
  }
  auto n = this->constantOf(run, "roundingIncrement");
  bool isKnown = n.has_value() || this->allIncrements(increment->type->nonOptional());
  bool isListed = !n.has_value() || this->isIncrement(*n);
  if (run.literal != nullptr && isKnown && n != 1.0 && isListed) {
    this->walkIncrementDigits(run);
  }
  if (!isKnown || !isListed) {
    auto message = ErrorTextIntlField::increments(n.has_value());
    this->reportOption(run, "roundingIncrement", message);
  }
}

void IntlOptionsChecker::walkIncrementDigits(IntlOptionsRun &run) {
  bool isSignificant = run.type->field("minimumSignificantDigits") != nullptr ||
                       run.type->field("maximumSignificantDigits") != nullptr;
  if (isSignificant) {
    auto message = ErrorTextIntlField::incSig();
    this->reportOption(run, "roundingIncrement", message);
    return;
  }
  bool isSameDigits = run.type->field("minimumFractionDigits") != nullptr &&
                      run.type->field("maximumFractionDigits") != nullptr &&
                      this->isNotAbove(run, "minimumFractionDigits", "maximumFractionDigits") &&
                      this->isNotAbove(run, "maximumFractionDigits", "minimumFractionDigits");
  if (!isSameDigits) {
    auto message = ErrorTextIntlField::incSame();
    this->reportOption(run, "roundingIncrement", message);
  }
}

}
