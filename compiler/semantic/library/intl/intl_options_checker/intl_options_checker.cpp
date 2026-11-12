#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/program/library/intl/intl_unit_table/intl_unit_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
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
  const auto *integer = dyn_cast<IntLitExpr>(expr);
  if (integer != nullptr) {
    return double(integer->value);
  }
  const auto *fraction = dyn_cast<FloatLitExpr>(expr);
  if (fraction != nullptr) {
    return fraction->value;
  }

  return std::nullopt;
}

bool IntlOptionsChecker::isKeywordType(llvm::StringRef text) const {
  auto parts = llvm::SmallVector<llvm::StringRef, 4>();
  text.split(parts, '-');
  auto isKeyword = std::bind_front(&IntlOptionsChecker::isKeywordPart, this);

  return llvm::all_of(parts, isKeyword);
}

bool IntlOptionsChecker::isKeywordPart(llvm::StringRef part) const {
  return part.size() >= 3 && part.size() <= 8 && llvm::all_of(part, llvm::isAlnum);
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
  auto isStringLiteral = std::bind_front(&IntlOptionsChecker::isStringLiteralMember, this);

  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() && llvm::all_of(type->members(), isStringLiteral));
}

bool IntlOptionsChecker::isStringLiteralMember(const Type *member) const {
  return member->isStringLiteral();
}

IntlOptionsChecker::ResultOptionsGiven
IntlOptionsChecker::checkOptionsGiven(const Type *type, IntlKind kind, llvm::StringRef what) const {
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

bool IntlOptionsChecker::prepareIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind,
                                         llvm::StringRef what) {
  const IntlService *service = kind == IntlKind::None
                                   ? &IntlServiceTable::getSupportedLocalesService()
                                   : IntlServiceTable::findKind(kind);
  bool isOk = true;
  if (args.size() > from) {
    Expr *&locale = args[from];
    locale = this->exprChecker->checkExpr(locale);
    isOk = this->intlChecker->prepareLocaleArg(locale, what);
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
  auto passed = this->checkOptionsGiven(type, kind, what);
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

const Field *IntlOptionsChecker::fieldOf(const IntlOptionsRun &run, IntlOptionKey key) const {
  auto name = IntlOptionKeyTable::getName(key);

  return run.type->field(name);
}

std::optional<std::string> IntlOptionsChecker::textOf(const IntlOptionsRun &run,
                                                      IntlOptionKey key) const {
  const auto *field = this->fieldOf(run, key);
  if (field == nullptr) {
    return std::nullopt;
  }
  auto *value = this->valueOf(run, IntlOptionKeyTable::getName(key));

  return this->knownText(value, field->type->nonOptional());
}

std::optional<double> IntlOptionsChecker::constantOf(const IntlOptionsRun &run,
                                                     IntlOptionKey key) const {
  auto *value = this->valueOf(run, IntlOptionKeyTable::getName(key));
  std::optional<double> literal = value != nullptr ? this->literalNumber(value) : std::nullopt;
  if (literal.has_value()) {
    return literal;
  }
  const auto *field = this->fieldOf(run, key);
  const Type *type = field != nullptr ? field->type->nonOptional() : nullptr;
  if (type != nullptr && type->isLiteral() && type->literalBase()->isNumeric()) {
    return type->literalNumber();
  }

  return std::nullopt;
}

const Expr *IntlOptionsChecker::valueExpr(const IntlOptionsRun &run, IntlOptionKey key) const {
  auto *value = this->valueOf(run, IntlOptionKeyTable::getName(key));

  return value != nullptr ? this->unconverted(value) : nullptr;
}

void IntlOptionsChecker::reportOption(IntlOptionsRun &run, IntlOptionKey key,
                                      const std::string &message) {
  auto name = IntlOptionKeyTable::getName(key);

  return this->reportOptionNamed(run, name, message);
}

void IntlOptionsChecker::reportOptionNamed(IntlOptionsRun &run, llvm::StringRef name,
                                           const std::string &message) {
  auto *value = this->valueOf(run, name);
  llvm::SMLoc at = value != nullptr ? value->loc() : run.options->loc();
  this->state.getDiagnostics().error(at, message);
  run.ok = false;
}

const IntlOption *IntlOptionsChecker::findOption(const IntlService *service,
                                                 llvm::StringRef name) const {
  auto key = IntlOptionKeyTable::find(name);
  if (!key.has_value()) {
    return nullptr;
  }
  for (const IntlOption &option : service->options) {
    if (option.key == *key) {
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
    this->reportOptionNamed(run, field.name, message);
    return;
  }
  if (value->isUndefined()) {
    return;
  }
  if (!this->isOptionKindFitting(found->kind, value)) {
    auto message = ErrorTextIntlField::kind(field.name, found->kind, field.type->name());
    this->reportOption(run, found->key, message);
    return;
  }
  auto text = this->textOf(run, found->key);
  bool isGrouping = found->kind == IntlOptionKind::GROUPING;
  bool isBadText = text.has_value() && !found->values.empty() &&
                   !llvm::is_contained(found->values, *text) && !(isGrouping && text->empty());
  if (isBadText) {
    auto message = ErrorTextIntlField::badValue(*text, field.name, found->values);
    this->reportOption(run, found->key, message);
    return;
  }
  bool isListed = found->kind != IntlOptionKind::NUMBER && found->kind != IntlOptionKind::FLAG;
  if (!text.has_value() && !found->values.empty() && isListed) {
    this->walkOptionValues(run, field, *found, value);
  }
}

bool IntlOptionsChecker::isOptionKindFitting(IntlOptionKind kind, const Type *value) const {
  switch (kind) {
    case IntlOptionKind::TEXT:
      return this->isStringLike(value);
    case IntlOptionKind::NUMBER:
      return this->isNumberLike(value) ||
             (value->isUnion() && this->areAllNumberLike(value->members()));
    case IntlOptionKind::FLAG:
      return this->isBooleanLike(value);
    case IntlOptionKind::GROUPING:
      break;
  }

  return this->isStringLike(value) || this->isBooleanLike(value) ||
         (value->isUnion() && this->areAllTextOrBoolean(value->members()));
}

bool IntlOptionsChecker::isBooleanLike(const Type *type) const {
  return type->isBool() || (type->isLiteral() && type->literalBase()->isBool());
}

bool IntlOptionsChecker::isNumberLike(const Type *type) const {
  return type->isNumeric() || (type->isLiteral() && type->literalBase()->isNumeric());
}

bool IntlOptionsChecker::areAllNumberLike(llvm::ArrayRef<const Type *> members) const {
  for (const Type *member : members) {
    if (!this->isNumberLike(member)) {
      return false;
    }
  }

  return true;
}

bool IntlOptionsChecker::areAllTextOrBoolean(llvm::ArrayRef<const Type *> members) const {
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
    this->reportOption(run, found.key, message);
  } else if (isOpen) {
    auto message = ErrorTextIntlField::open(field.name, found.values);
    this->reportOption(run, found.key, message);
  }
}

const std::string *IntlOptionsChecker::wrongText(const std::vector<std::string> &texts,
                                                 const IntlOption &found) const {
  bool isGrouping = found.kind == IntlOptionKind::GROUPING;
  for (const std::string &text : texts) {
    if (!llvm::is_contained(found.values, text) && !(isGrouping && text.empty())) {
      return &text;
    }
  }

  return nullptr;
}

void IntlOptionsChecker::walkOptionTexts(IntlOptionsRun &run) {
  auto code = this->textOf(run, IntlOptionKey::CURRENCY);
  if (code.has_value() && !(code->size() == 3 && llvm::all_of(*code, llvm::isAlpha))) {
    auto message = ErrorTextIntlField::currency(*code);
    this->reportOption(run, IntlOptionKey::CURRENCY, message);
  }
  auto unit = this->textOf(run, IntlOptionKey::UNIT);
  if (unit.has_value() && !IntlUnitTable::isWellFormed(*unit)) {
    auto message = ErrorTextIntlField::unit(*unit);
    this->reportOption(run, IntlOptionKey::UNIT, message);
  }
  for (IntlOptionKey key : {IntlOptionKey::NUMBERING_SYSTEM, IntlOptionKey::COLLATION}) {
    auto value = this->textOf(run, key);
    if (value.has_value() && !this->isKeywordType(*value)) {
      auto name = IntlOptionKeyTable::getName(key);
      auto message = ErrorTextIntlField::keyword(*value, name);
      this->reportOption(run, key, message);
    }
  }
  this->walkStyleOption(run);
  if (run.kind == IntlKind::DisplayNames && this->fieldOf(run, IntlOptionKey::TYPE) == nullptr) {
    auto message = ErrorTextIntlOptions::noType();
    this->reportOption(run, IntlOptionKey::TYPE, message);
  }
}

void IntlOptionsChecker::walkStyleOption(IntlOptionsRun &run) {
  const auto *style = this->fieldOf(run, IntlOptionKey::STYLE);
  if (style == nullptr || run.kind != IntlKind::NumberFormat) {
    return;
  }
  bool isOpen = false;
  auto styles = this->literalTexts(style->type, isOpen);
  auto text = this->textOf(run, IntlOptionKey::STYLE);
  if (text.has_value()) {
    styles = {*text};
  }
  auto currencyStyle = IntlOptionKeyTable::getName(IntlOptionKey::CURRENCY);
  auto unitStyle = IntlOptionKeyTable::getName(IntlOptionKey::UNIT);
  bool hasCurrency = this->fieldOf(run, IntlOptionKey::CURRENCY) != nullptr;
  bool hasUnit = this->fieldOf(run, IntlOptionKey::UNIT) != nullptr;
  if (llvm::is_contained(styles, currencyStyle) && !hasCurrency) {
    auto message = ErrorTextIntlField::styleCur();
    this->reportOption(run, IntlOptionKey::STYLE, message);
  }
  if (llvm::is_contained(styles, unitStyle) && !hasUnit) {
    auto message = ErrorTextIntlField::styleUnit();
    this->reportOption(run, IntlOptionKey::STYLE, message);
  }
}

void IntlOptionsChecker::walkDigitRanges(IntlOptionsRun &run) {
  for (IntlOptionKey key :
       {IntlOptionKey::MINIMUM_INTEGER_DIGITS, IntlOptionKey::MINIMUM_FRACTION_DIGITS,
        IntlOptionKey::MAXIMUM_FRACTION_DIGITS, IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS,
        IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS}) {
    auto range = IntlServiceTable::findDigitRange(key);
    this->walkDigitRange(run, *range);
  }
}

void IntlOptionsChecker::walkDigitRange(IntlOptionsRun &run, const IntlDigitRange &range) {
  const auto *field = this->fieldOf(run, range.key);
  if (field == nullptr) {
    return;
  }
  auto name = IntlOptionKeyTable::getName(range.key);
  int min = int(range.low);
  int max = int(range.high);
  auto constant = this->constantOf(run, range.key);
  if (constant.has_value() && !(*constant >= min && *constant <= max)) {
    auto message = ErrorTextIntlField::range(name, min, max);
    this->reportOption(run, range.key, message);
  }
  if (constant.has_value()) {
    return;
  }
  auto held = this->literalTypes->intRange(field->type->nonOptional());
  if (held.has_value() && held->first >= min && held->second <= max) {
    return;
  }
  const auto *value = this->valueExpr(run, range.key);
  if (value != nullptr && value->type->isInt() && this->byteProofs->canProveFits(value, min, max)) {
    return;
  }
  auto message = ErrorTextIntlField::rangeOpen(name, min, max);
  this->reportOption(run, range.key, message);
}

bool IntlOptionsChecker::isNotAbove(const IntlOptionsRun &run, IntlOptionKey low,
                                    IntlOptionKey high) {
  auto lowConstant = this->constantOf(run, low);
  auto highConstant = this->constantOf(run, high);
  if (lowConstant.has_value() && highConstant.has_value()) {
    return std::floor(*lowConstant) <= std::floor(*highConstant);
  }
  auto lowLinear = this->optionLinear(run, low, lowConstant);
  auto highLinear = this->optionLinear(run, high, highConstant);
  if (!lowLinear.has_value() || !highLinear.has_value()) {
    return false;
  }
  if (lowLinear->term == highLinear->term) {
    return lowLinear->offset <= highLinear->offset;
  }
  int64_t gap = highLinear->offset - lowLinear->offset + 1;

  return this->state.getFlowState().getFacts().canProve(lowLinear->term, highLinear->term, gap);
}

std::optional<Linear> IntlOptionsChecker::optionLinear(const IntlOptionsRun &run, IntlOptionKey key,
                                                       std::optional<double> constant) {
  const auto *value = this->valueExpr(run, key);
  std::optional<Linear> linear = value != nullptr ? this->indexProofs->linear(value) : std::nullopt;
  if (linear.has_value()) {
    return linear;
  }
  if (constant.has_value() && *constant == std::floor(*constant)) {
    return Linear{Term::zero(), int64_t(*constant)};
  }

  return std::nullopt;
}

void IntlOptionsChecker::walkDigitPairs(IntlOptionsRun &run) {
  static const std::pair<IntlOptionKey, IntlOptionKey> pairs[] = {
      {IntlOptionKey::MINIMUM_FRACTION_DIGITS, IntlOptionKey::MAXIMUM_FRACTION_DIGITS},
      {IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS, IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS}};
  for (const auto &[min, max] : pairs) {
    bool isSkipped = run.literal == nullptr || this->fieldOf(run, min) == nullptr ||
                     this->fieldOf(run, max) == nullptr || this->isNotAbove(run, min, max);
    if (!isSkipped) {
      auto low = this->constantOf(run, min);
      auto high = this->constantOf(run, max);
      auto minName = IntlOptionKeyTable::getName(min);
      auto maxName = IntlOptionKeyTable::getName(max);
      auto message =
          ErrorTextIntlField::above(minName, maxName, low.has_value() && high.has_value());
      this->reportOption(run, max, message);
    }
  }
}

bool IntlOptionsChecker::isIncrement(double value) const {
  return llvm::is_contained(IntlServiceTable::getRoundingIncrements(), value);
}

bool IntlOptionsChecker::areAllIncrements(const Type *type) const {
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
  const auto *increment = this->fieldOf(run, IntlOptionKey::ROUNDING_INCREMENT);
  if (increment == nullptr) {
    return;
  }
  auto constant = this->constantOf(run, IntlOptionKey::ROUNDING_INCREMENT);
  bool isKnown = constant.has_value() || this->areAllIncrements(increment->type->nonOptional());
  bool isListed = !constant.has_value() || this->isIncrement(*constant);
  if (run.literal != nullptr && isKnown && constant != 1.0 && isListed) {
    this->walkIncrementDigits(run);
  }
  if (!isKnown || !isListed) {
    auto message = ErrorTextIntlField::increments(constant.has_value());
    this->reportOption(run, IntlOptionKey::ROUNDING_INCREMENT, message);
  }
}

void IntlOptionsChecker::walkIncrementDigits(IntlOptionsRun &run) {
  bool isSignificant = this->fieldOf(run, IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS) != nullptr ||
                       this->fieldOf(run, IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS) != nullptr;
  if (isSignificant) {
    auto message = ErrorTextIntlField::incSig();
    this->reportOption(run, IntlOptionKey::ROUNDING_INCREMENT, message);
    return;
  }
  IntlOptionKey low = IntlOptionKey::MINIMUM_FRACTION_DIGITS;
  IntlOptionKey high = IntlOptionKey::MAXIMUM_FRACTION_DIGITS;
  bool isSameDigits = this->fieldOf(run, low) != nullptr && this->fieldOf(run, high) != nullptr &&
                      this->isNotAbove(run, low, high) && this->isNotAbove(run, high, low);
  if (!isSameDigits) {
    auto message = ErrorTextIntlField::incSame();
    this->reportOption(run, IntlOptionKey::ROUNDING_INCREMENT, message);
  }
}

}
