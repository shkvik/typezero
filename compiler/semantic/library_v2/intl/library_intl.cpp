#include "compiler/semantic/library_v2/intl/library_intl.h"
#include "compiler/program/library/date/date_format_option_table/date_format_option_table.h"
#include "compiler/program/library/intl/intl_option_key_table/intl_option_key_table.h"
#include "compiler/program/library/intl/intl_shape_table/intl_shape_table.h"
#include "compiler/program/library/intl/intl_type_name_table/intl_type_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"

namespace tz {

namespace {

constexpr int64_t kFewestDigits = 1;
constexpr int64_t kMostDigits = 3;

}

LibraryIntl::LibraryIntl(SemanticState &owner)
    : state(owner) {}

const Type *LibraryIntl::numberOptionType(IntlOptionKey key) const {
  bool isIncrement = key == IntlOptionKey::ROUNDING_INCREMENT;
  auto increments = IntlServiceTable::getRoundingIncrements();
  auto values = isIncrement ? std::vector<int64_t>(increments.begin(), increments.end())
                            : this->digitRange(key);
  auto members = std::vector<const Type *>();
  for (int64_t value : values) {
    const auto *literal = Type::numberLiteral(double(value), Type::intTy());
    members.push_back(literal);
  }
  const auto *type = Type::unionOf(members);
  if (!isIncrement) {
    auto rangeName = std::to_string(values.front()) + ".." + std::to_string(values.back());
    Type::nameUnion(type, rangeName);
  }

  return type;
}

std::vector<int64_t> LibraryIntl::digitRange(IntlOptionKey key) const {
  auto range = IntlServiceTable::findDigitRange(key);
  auto values = std::vector<int64_t>();
  for (int64_t value = range->low; value <= range->high; ++value) {
    values.push_back(value);
  }

  return values;
}

const Type *LibraryIntl::segmentDataType() const {
  return this->shapeType(IntlShape::SEGMENT_DATA);
}

const Type *LibraryIntl::shapeType(IntlShape shape) const {
  const IntlShapeInfo &info = IntlShapeTable::find(shape);
  auto fields = std::vector<Field>();
  for (const IntlShapeField &field : info.fields) {
    const Type *type = this->shapeValueType(field.type);
    fields.push_back({field.name.str(), type, false});
  }

  return Type::object(std::move(fields), info.typeName);
}

const Type *LibraryIntl::shapeValueType(IntlValueType type) const {
  switch (type) {
    case IntlValueType::TEXT:
      return Type::string();
    case IntlValueType::MAYBE_TEXT:
      return Type::optional(Type::string());
    case IntlValueType::FLAG:
      return Type::boolTy();
    case IntlValueType::MAYBE_FLAG:
      return Type::optional(Type::boolTy());
    case IntlValueType::INT:
      return Type::intTy();
    case IntlValueType::MAYBE_INT:
      return Type::optional(Type::intTy());
    case IntlValueType::TEXT_OR_FLAG:
      return Type::unionOf({Type::string(), Type::boolTy()});
    case IntlValueType::TEXT_LIST:
      return Type::array(Type::string());
  }

  return Type::error();
}

const Type *LibraryIntl::resolvedIntlOptionsType(IntlKind kind) const {
  return this->shapeType(IntlShapeTable::resolvedOf(kind));
}

const Type *LibraryIntl::intlTypeNamed(llvm::StringRef name) const {
  const auto *service = IntlServiceTable::find(name);
  if (service != nullptr) {
    return Type::intl(service->kind);
  }
  auto typeName = IntlTypeNameTable::find(name);
  if (!typeName.has_value()) {
    return this->serviceTypeNamed(name);
  }
  switch (*typeName) {
    case IntlTypeName::DATE_TIME_FORMAT:
      return Type::dateFormat();
    case IntlTypeName::SEGMENTS:
      return Type::intl(IntlKind::Segments);
    case IntlTypeName::SEGMENT_DATA:
      return this->segmentDataType();
    case IntlTypeName::DATE_TIME_FORMAT_PART:
    case IntlTypeName::NUMBER_FORMAT_PART:
      return this->shapeType(IntlShape::DATE_TIME_FORMAT_PART);
    case IntlTypeName::RELATIVE_TIME_FORMAT_PART:
      return this->shapeType(IntlShape::RELATIVE_TIME_FORMAT_PART);
    case IntlTypeName::RESOLVED_DATE_TIME_FORMAT_OPTIONS:
      return this->shapeType(IntlShape::RESOLVED_DATE_TIME_FORMAT);
    case IntlTypeName::DATE_TIME_FORMAT_OPTIONS:
      return this->dateOptionsType();
    case IntlTypeName::LDML_PLURAL_RULE:
    case IntlTypeName::RELATIVE_TIME_FORMAT_UNIT:
    case IntlTypeName::UNICODE_BCP47_LOCALE_IDENTIFIER:
    case IntlTypeName::BCP47_LANGUAGE_TAG:
      return Type::string();
    case IntlTypeName::LOCALE:
      break;
  }

  return this->serviceTypeNamed(name);
}

const Type *LibraryIntl::serviceTypeNamed(llvm::StringRef name) const {
  const auto *resolved = IntlServiceTable::findByResolvedTypeName(name);
  if (resolved != nullptr) {
    return this->resolvedIntlOptionsType(resolved->kind);
  }
  const auto *options = IntlServiceTable::findByOptionsTypeName(name);
  if (options != nullptr) {
    return this->serviceOptionsType(*options);
  }

  return nullptr;
}

const Type *LibraryIntl::serviceOptionsType(const IntlService &service) const {
  auto fields = std::vector<Field>();
  for (const IntlOption &option : service.options) {
    const auto *type = this->optionType(option);
    auto name = IntlOptionKeyTable::getName(option.key);
    fields.push_back({.name = name.str(), .type = Type::optional(type)});
  }
  auto typeName = IntlServiceTable::getOptionsTypeName(service.name);

  return Type::object(std::move(fields), typeName);
}

const Type *LibraryIntl::optionType(const IntlOption &option) const {
  if (option.kind == IntlOptionKind::NUMBER) {
    return this->numberOptionType(option.key);
  }
  bool isTextual = option.kind == IntlOptionKind::TEXT || option.kind == IntlOptionKind::GROUPING;
  if (isTextual && !option.values.empty()) {
    return this->listedOptionType(option);
  }
  if (option.kind == IntlOptionKind::TEXT) {
    return Type::string();
  }
  if (option.kind == IntlOptionKind::FLAG) {
    return Type::boolTy();
  }

  return Type::unionOf({Type::string(), Type::boolTy()});
}

const Type *LibraryIntl::listedOptionType(const IntlOption &option) const {
  auto members = std::vector<const Type *>();
  for (llvm::StringLiteral value : option.values) {
    auto text = ToUtf16::toUtf16(value);
    members.push_back(Type::stringLiteral(text));
  }
  if (option.kind == IntlOptionKind::GROUPING) {
    members.push_back(Type::boolTy());
  }

  return Type::unionOf(members);
}

const Type *LibraryIntl::dateOptionsType() const {
  auto fields = std::vector<Field>();
  for (const DateFormatOptionInfo &option : DateFormatOptionTable::getOptions()) {
    const Type *value = this->dateOptionValueType(option);
    fields.push_back({option.name.str(), Type::optional(value), false});
  }
  auto serviceName = IntlTypeNameTable::getName(IntlTypeName::DATE_TIME_FORMAT);
  auto typeName = IntlServiceTable::getOptionsTypeName(serviceName);

  return Type::object(std::move(fields), typeName);
}

const Type *LibraryIntl::dateOptionValueType(const DateFormatOptionInfo &option) const {
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

const Type *LibraryIntl::dateFormatPartType() const {
  return this->shapeType(IntlShape::DATE_TIME_FORMAT_PART);
}

const Type *LibraryIntl::resolvedDateOptionsType() const {
  return this->shapeType(IntlShape::RESOLVED_DATE_TIME_FORMAT);
}

}
