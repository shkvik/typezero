#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/date/date_format_method_table/date_format_method_table.h"
#include "compiler/program/library/date/date_format_option_table/date_format_option_table.h"
#include "compiler/program/library/intl/intl_type_name_table/intl_type_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr int64_t kFewestDigits = 1;
constexpr int64_t kMostDigits = 3;


}

DateFormatChecker::DateFormatChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->intlChecker = &all.intlChecker;
  this->literalTypes = &all.literalTypes;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
}

std::optional<std::u16string> DateFormatChecker::literalText(const Expr *expr) const {
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

std::optional<std::vector<std::string>> DateFormatChecker::textsOf(const Type *type) const {
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  auto texts = std::vector<std::string>();
  for (const Type *member : members) {
    if (!member->isStringLiteral()) {
      return std::nullopt;
    }
    texts.push_back(ToUtf8::toUtf8(member->literalText()));
  }

  return texts;
}

std::optional<std::vector<double>> DateFormatChecker::numbersOf(const Type *type) const {
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  auto numbers = std::vector<double>();
  for (const Type *member : members) {
    if (!member->isLiteral() || !member->literalBase()->isNumeric()) {
      return std::nullopt;
    }
    numbers.push_back(member->literalNumber());
  }

  return numbers;
}

bool DateFormatChecker::isLanguageTag(const std::string &tag) const {
  auto parts = llvm::SmallVector<llvm::StringRef, 4>();
  llvm::StringRef(tag).split(parts, '-');
  for (llvm::StringRef part : parts) {
    if (part.empty() || part.size() > 8 || !llvm::all_of(part, llvm::isAlnum)) {
      return false;
    }
  }
  llvm::StringRef language = parts[0];
  bool isLength = (language.size() >= 2 && language.size() <= 3) ||
                  (language.size() >= 5 && language.size() <= 8);

  return isLength && llvm::all_of(language, llvm::isAlpha);
}

DateFormatChecker::ResultOptionsObject DateFormatChecker::checkOptionsObject(const Type *type,
                                                                             int kind) const {
  if (!type->isObject() || type->isTuple() || type->isClass() || type->isRegExp() ||
      type->isMatch() || type->isDate() || type->isIntl() || type->isCrypto() || type->isBytes()) {
    auto message =
        ErrorTextDateOptions::notObject(ErrorTextDateOptions::method(kind), type->name());

    return err(DateFormatCheckerOptionsObjectError{
        .code = DateFormatCheckerOptionsObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

bool DateFormatChecker::prepareLocaleArgs(std::vector<Expr *> &args, int kind) {
  bool isOk = true;
  if (!args.empty()) {
    args[0] = this->exprChecker->checkExpr(args[0]);
    isOk = this->intlChecker->prepareLocaleArg(args[0], ErrorTextDateOptions::method(kind));
  }
  if (args.size() < 2) {
    return isOk;
  }
  Expr *&options = args[1];
  options = this->exprChecker->checkExpr(options);
  const Type *type = options->type;
  if (type->isError()) {
    return false;
  }
  if (type->isUndefined()) {
    return isOk;
  }
  auto object = this->checkOptionsObject(type, kind);
  if (object.isErr()) {
    this->state.getDiagnostics().error(options->loc(), object.error().message);
    return false;
  }
  bool isKindFitting = this->matchOptionKinds(options, kind);
  bool isValuesFitting = this->matchOptionValues(options);

  return isOk && isKindFitting && isValuesFitting;
}

bool DateFormatChecker::matchOptionKinds(const Expr *options, int kind) {
  bool isOk = true;
  auto roles = OptionRoles{.hasStyle = false, .hasComponent = false};
  for (const Field &field : options->type->fields()) {
    auto option = DateFormatOptionTable::find(field.name);
    bool isFitting = this->matchOptionKind(options, field, option, kind);
    isOk &= isFitting;
    if (isFitting) {
      isOk &= this->matchOptionRole(options, field, option.value(), kind, roles);
    }
  }
  if (roles.hasStyle && roles.hasComponent) {
    auto message = ErrorTextDateOptions::styleParts();
    this->state.getDiagnostics().error(options->loc(), message);
    isOk = false;
  }

  return isOk;
}

bool DateFormatChecker::matchOptionRole(const Expr *options, const Field &field,
                                        DateFormatOption option, int kind, OptionRoles &roles) {
  bool isPresent = !field.type->isOptional();
  auto role = this->optionRole(option);
  if (role == DateOptionRole::Component) {
    roles.hasComponent |= isPresent;
    return true;
  }
  if (role != DateOptionRole::Style) {
    return true;
  }
  roles.hasStyle |= isPresent;

  return !isPresent || this->matchStyleMethod(options, field.name, kind);
}

bool DateFormatChecker::matchOptionKind(const Expr *options, const Field &field,
                                        std::optional<DateFormatOption> option, int kind) {
  if (!option.has_value()) {
    auto message = ErrorTextDateOptions::unknown(field.name, ErrorTextDateOptions::method(kind));
    this->state.getDiagnostics().error(options->loc(), message);
    return false;
  }
  const Type *value = field.type->nonOptional();
  const auto *wanted = this->wantedOptionType(*option, value);
  if (value == wanted) {
    return true;
  }
  auto message = ErrorTextDateOptions::kind(field.name, wanted == Type::boolTy(),
                                            wanted->isNumeric(), field.type->name());
  this->state.getDiagnostics().error(options->loc(), message);

  return false;
}

const Type *DateFormatChecker::wantedOptionType(DateFormatOption option, const Type *value) const {
  if (option == DateFormatOption::HOUR12) {
    return Type::boolTy();
  }
  if (option == DateFormatOption::FRACTIONAL_SECOND_DIGITS) {
    bool isNumeric = value->isNumeric() || this->numbersOf(value).has_value();
    return isNumeric ? value : Type::intTy();
  }
  if (this->textsOf(value).has_value()) {
    return value;
  }

  return Type::string();
}

DateFormatChecker::DateOptionRole DateFormatChecker::optionRole(DateFormatOption option) const {
  switch (option) {
    case DateFormatOption::DATE_STYLE:
    case DateFormatOption::TIME_STYLE:
      return DateOptionRole::Style;
    case DateFormatOption::WEEKDAY:
    case DateFormatOption::ERA:
    case DateFormatOption::YEAR:
    case DateFormatOption::MONTH:
    case DateFormatOption::DAY:
    case DateFormatOption::DAY_PERIOD:
    case DateFormatOption::HOUR:
    case DateFormatOption::MINUTE:
    case DateFormatOption::SECOND:
    case DateFormatOption::TIME_ZONE_NAME:
    case DateFormatOption::FRACTIONAL_SECOND_DIGITS:
      return DateOptionRole::Component;
    case DateFormatOption::LOCALE_MATCHER:
    case DateFormatOption::CALENDAR:
    case DateFormatOption::NUMBERING_SYSTEM:
    case DateFormatOption::HOUR_CYCLE:
    case DateFormatOption::TIME_ZONE:
    case DateFormatOption::FORMAT_MATCHER:
    case DateFormatOption::HOUR12:
      return DateOptionRole::Setting;
  }

  return DateOptionRole::Setting;
}

bool DateFormatChecker::matchStyleMethod(const Expr *options, llvm::StringRef name, int kind) {
  auto option = DateFormatOptionTable::find(name);
  bool isForeign = (kind == TZ_DATE_LOCALE_DATE && option == DateFormatOption::TIME_STYLE) ||
                   (kind == TZ_DATE_LOCALE_TIME && option == DateFormatOption::DATE_STYLE);
  if (!isForeign) {
    return true;
  }
  auto message = ErrorTextDateOptions::styleOnly(name, ErrorTextDateOptions::method(kind));
  this->state.getDiagnostics().error(options->loc(), message);

  return false;
}

bool DateFormatChecker::matchOptionValues(const Expr *options) {
  const auto *literal = dyn_cast<ObjectLitExpr>(options);
  bool isExempt = this->indexProofs->isExempt(options->loc());
  bool isOk = true;
  for (const Field &field : options->type->fields()) {
    const auto *given = this->givenProperty(literal, field.name);
    DateOptionPlace place{
        .given = given,
        .at = given != nullptr ? given->loc() : options->loc(),
        .isExempt = isExempt,
    };
    auto option = DateFormatOptionTable::find(field.name);
    bool hasDigits = option == DateFormatOption::FRACTIONAL_SECOND_DIGITS;
    bool isFitting =
        hasDigits ? this->matchDigits(field, place) : this->matchOptionText(field, place);
    isOk &= isFitting;
  }

  return isOk;
}

const Expr *DateFormatChecker::givenProperty(const ObjectLitExpr *literal,
                                             llvm::StringRef name) const {
  if (literal == nullptr) {
    return nullptr;
  }
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && property.name == name && property.value != nullptr) {
      return property.value;
    }
  }

  return nullptr;
}

bool DateFormatChecker::matchDigits(const Field &field, const DateOptionPlace &place) {
  if (place.given != nullptr) {
    auto digits = ErrorTextDateOptions::digitsName();
    auto between =
        this->indexProofs->requireBetween(place.given, kFewestDigits, kMostDigits, digits);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(place.given->loc(), between.error().message);
    }
    return between.isOk();
  }
  auto digits = this->numbersOf(field.type->nonOptional());
  auto isOutside = std::bind_front(&DateFormatChecker::isOutsideDigits, this);
  bool isAnyOutside = !digits.has_value() || llvm::any_of(*digits, isOutside);
  if (place.isExempt || !isAnyOutside) {
    return true;
  }
  auto message = ErrorTextDateOptions::digits();
  this->state.getDiagnostics().error(place.at, message);

  return false;
}

bool DateFormatChecker::isOutsideDigits(double digits) const {
  return digits < kFewestDigits || digits > kMostDigits;
}

bool DateFormatChecker::matchOptionText(const Field &field, const DateOptionPlace &place) {
  auto values = DateFormatOptionTable::getValues(field.name);
  if (values.empty() || place.isExempt) {
    return true;
  }
  auto texts = this->optionTexts(field, place);
  if (!texts.has_value()) {
    std::string written =
        place.given != nullptr ? BytesCheckHelpers::valueText(place.given) : std::string();
    auto message = ErrorTextDateOptions::unknownVal(field.name, written, values[0], values[1]);
    this->state.getDiagnostics().error(place.at, message);
    return false;
  }
  for (const std::string &text : *texts) {
    if (!llvm::is_contained(values, text)) {
      auto message = ErrorTextDateOptions::badValue(text, field.name, values);
      this->state.getDiagnostics().error(place.at, message);
      return false;
    }
  }

  return true;
}

std::optional<std::vector<std::string>>
DateFormatChecker::optionTexts(const Field &field, const DateOptionPlace &place) {
  if (place.given != nullptr) {
    auto known = this->literalTypes->knownTexts(place.given);
    if (known.has_value()) {
      return known;
    }
  }

  return this->textsOf(field.type->nonOptional());
}

const Type *DateFormatChecker::dateOptionsType() const {
  auto fields = std::vector<Field>();
  for (const DateFormatOptionInfo &option : DateFormatOptionTable::getOptions()) {
    const Type *value = this->dateOptionValueType(option);
    fields.push_back({option.name.str(), Type::optional(value), false});
  }
  auto serviceName = IntlTypeNameTable::getName(IntlTypeName::DATE_TIME_FORMAT);
  auto typeName = IntlServiceTable::getOptionsTypeName(serviceName);

  return Type::object(std::move(fields), typeName);
}

const Type *DateFormatChecker::dateOptionValueType(const DateFormatOptionInfo &option) const {
  if (option.value == DateFormatOption::HOUR12) {
    return Type::boolTy();
  }
  if (option.value == DateFormatOption::FRACTIONAL_SECOND_DIGITS) {
    auto digits = std::vector<const Type *>();
    for (int64_t count = kFewestDigits; count <= kMostDigits; ++count) {
      digits.push_back(Type::numberLiteral(double(count), Type::intTy()));
    }
    return Type::unionOf(digits);
  }
  if (option.values.empty()) {
    return Type::string();
  }
  auto literals = std::vector<const Type *>();
  for (llvm::StringLiteral text : option.values) {
    literals.push_back(Type::stringLiteral(std::u16string(text.begin(), text.end())));
  }

  return Type::unionOf(literals);
}

const Type *DateFormatChecker::dateFormatPartType() const {
  return this->intlChecker->shapeType(IntlShape::DATE_TIME_FORMAT_PART);
}

const Type *DateFormatChecker::resolvedDateOptionsType() const {
  return this->intlChecker->shapeType(IntlShape::RESOLVED_DATE_TIME_FORMAT);
}

Expr *DateFormatChecker::checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto intlName = GlobalNameTable::getName(GlobalName::INTL);
  auto *callee = this->state.getAst().make<NameExpr>(loc, intlName);
  if (args.size() > 2) {
    auto message = ErrorTextDateFormat::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    for (Expr *&arg : args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  if (!this->prepareLocaleArgs(args, TZ_DATE_LOCALE_FORMAT)) {
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }

  return this->processChecker->builtinCall(loc, callee, Builtin::DtfNew, args, Type::dateFormat());
}

Expr *DateFormatChecker::checkIntlCall(CallExpr *call, MemberExpr *member) {
  if (IntlTypeNameTable::find(member->name) == IntlTypeName::DATE_TIME_FORMAT) {
    if (this->callChecker->rejectSpread(call)) {
      return call;
    }
    return this->checkDateFormatNew(member->loc(), call->args);
  }

  return this->intlChecker->checkIntlFunction(call, member);
}

Expr *DateFormatChecker::checkDateFormatMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto method = DateFormatMethodTable::find(name);
  bool isFormat = method == DateFormatMethod::FORMAT;
  if (isFormat || method == DateFormatMethod::FORMAT_TO_PARTS) {
    auto arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      Expr *&arg = call->args[0];
      arg = this->genericChecker->apparent(this->exprChecker->checkExpr(arg));
      if (arg->type->isNumeric()) {
        arg = this->typeRelations->convert(arg, Type::doubleTy());
      } else if (!arg->type->isDate() && !arg->type->isError()) {
        auto message = ErrorTextDateFormat::formatArg(name, arg->type->name());
        this->state.getDiagnostics().error(arg->loc(), message);
        return call;
      }
    }
    call->target =
        BuiltinCall{.builtin = isFormat ? Builtin::DtfFormat : Builtin::DtfFormatToParts};
    call->type = isFormat ? Type::string() : Type::array(this->dateFormatPartType());
    return call;
  }
  if (method == DateFormatMethod::RESOLVED_OPTIONS) {
    auto arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = Builtin::DtfResolvedOptions};
    call->type = this->resolvedDateOptionsType();
    return call;
  }
  bool isRange =
      method == DateFormatMethod::FORMAT_RANGE || method == DateFormatMethod::FORMAT_RANGE_TO_PARTS;
  std::string message =
      isRange ? ErrorTextDateFormat::range(name) : ErrorTextDateFormat::noMember(name);
  this->state.getDiagnostics().error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

}
