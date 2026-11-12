#include "compiler/program/library/intl/intl_shape_table/intl_shape_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kLocaleTagField = "tag";

const IntlShapeInfo kShapes[] = {
    {
        .shape = IntlShape::RELATIVE_TIME_FORMAT_PART,
        .typeName = "Intl.RelativeTimeFormatPart",
        .fields = {{"type", IntlValueType::TEXT},
                   {"value", IntlValueType::TEXT},
                   {"unit", IntlValueType::MAYBE_TEXT}},
    },
    {
        .shape = IntlShape::SEGMENT_DATA,
        .typeName = "Intl.SegmentData",
        .fields = {{"segment", IntlValueType::TEXT},
                   {"index", IntlValueType::INT},
                   {"input", IntlValueType::TEXT},
                   {"isWordLike", IntlValueType::MAYBE_FLAG}},
    },
    {
        .shape = IntlShape::DATE_TIME_FORMAT_PART,
        .typeName = "Intl.DateTimeFormatPart",
        .fields = {{"type", IntlValueType::TEXT}, {"value", IntlValueType::TEXT}},
    },
    {
        .shape = IntlShape::RESOLVED_NUMBER_FORMAT,
        .typeName = "Intl.ResolvedNumberFormatOptions",
        .fields = {{"locale", IntlValueType::TEXT},
                   {"numberingSystem", IntlValueType::TEXT},
                   {"style", IntlValueType::TEXT},
                   {"currency", IntlValueType::MAYBE_TEXT},
                   {"currencyDisplay", IntlValueType::MAYBE_TEXT},
                   {"currencySign", IntlValueType::MAYBE_TEXT},
                   {"unit", IntlValueType::MAYBE_TEXT},
                   {"unitDisplay", IntlValueType::MAYBE_TEXT},
                   {"minimumIntegerDigits", IntlValueType::INT},
                   {"minimumFractionDigits", IntlValueType::MAYBE_INT},
                   {"maximumFractionDigits", IntlValueType::MAYBE_INT},
                   {"minimumSignificantDigits", IntlValueType::MAYBE_INT},
                   {"maximumSignificantDigits", IntlValueType::MAYBE_INT},
                   {"useGrouping", IntlValueType::TEXT_OR_FLAG},
                   {"notation", IntlValueType::TEXT},
                   {"compactDisplay", IntlValueType::MAYBE_TEXT},
                   {"signDisplay", IntlValueType::TEXT},
                   {"roundingIncrement", IntlValueType::INT},
                   {"roundingMode", IntlValueType::TEXT},
                   {"roundingPriority", IntlValueType::TEXT},
                   {"trailingZeroDisplay", IntlValueType::TEXT}},
    },
    {
        .shape = IntlShape::RESOLVED_COLLATOR,
        .typeName = "Intl.ResolvedCollatorOptions",
        .fields = {{"locale", IntlValueType::TEXT},
                   {"usage", IntlValueType::TEXT},
                   {"sensitivity", IntlValueType::TEXT},
                   {"ignorePunctuation", IntlValueType::FLAG},
                   {"collation", IntlValueType::TEXT},
                   {"numeric", IntlValueType::FLAG},
                   {"caseFirst", IntlValueType::TEXT}},
    },
    {
        .shape = IntlShape::RESOLVED_PLURAL_RULES,
        .typeName = "Intl.ResolvedPluralRulesOptions",
        .fields = {{"locale", IntlValueType::TEXT},
                   {"type", IntlValueType::TEXT},
                   {"minimumIntegerDigits", IntlValueType::INT},
                   {"minimumFractionDigits", IntlValueType::MAYBE_INT},
                   {"maximumFractionDigits", IntlValueType::MAYBE_INT},
                   {"minimumSignificantDigits", IntlValueType::MAYBE_INT},
                   {"maximumSignificantDigits", IntlValueType::MAYBE_INT},
                   {"pluralCategories", IntlValueType::TEXT_LIST},
                   {"roundingIncrement", IntlValueType::INT},
                   {"roundingMode", IntlValueType::TEXT},
                   {"roundingPriority", IntlValueType::TEXT},
                   {"trailingZeroDisplay", IntlValueType::TEXT}},
    },
    {
        .shape = IntlShape::RESOLVED_RELATIVE_TIME_FORMAT,
        .typeName = "Intl.ResolvedRelativeTimeFormatOptions",
        .fields = {{"locale", IntlValueType::TEXT},
                   {"style", IntlValueType::TEXT},
                   {"numeric", IntlValueType::TEXT},
                   {"numberingSystem", IntlValueType::TEXT}},
    },
    {
        .shape = IntlShape::RESOLVED_LIST_FORMAT,
        .typeName = "Intl.ResolvedListFormatOptions",
        .fields = {{"locale", IntlValueType::TEXT},
                   {"type", IntlValueType::TEXT},
                   {"style", IntlValueType::TEXT}},
    },
    {
        .shape = IntlShape::RESOLVED_DISPLAY_NAMES,
        .typeName = "Intl.ResolvedDisplayNamesOptions",
        .fields = {{"locale", IntlValueType::TEXT},
                   {"style", IntlValueType::TEXT},
                   {"type", IntlValueType::TEXT},
                   {"fallback", IntlValueType::TEXT},
                   {"languageDisplay", IntlValueType::MAYBE_TEXT}},
    },
    {
        .shape = IntlShape::RESOLVED_SEGMENTER,
        .typeName = "Intl.ResolvedSegmenterOptions",
        .fields = {{"locale", IntlValueType::TEXT}, {"granularity", IntlValueType::TEXT}},
    },
    {
        .shape = IntlShape::RESOLVED_DATE_TIME_FORMAT,
        .typeName = "Intl.ResolvedDateTimeFormatOptions",
        .fields = {{"locale", IntlValueType::TEXT},
                   {"calendar", IntlValueType::TEXT},
                   {"numberingSystem", IntlValueType::TEXT},
                   {"timeZone", IntlValueType::TEXT},
                   {"hourCycle", IntlValueType::MAYBE_TEXT},
                   {"hour12", IntlValueType::MAYBE_FLAG},
                   {"weekday", IntlValueType::MAYBE_TEXT},
                   {"era", IntlValueType::MAYBE_TEXT},
                   {"year", IntlValueType::MAYBE_TEXT},
                   {"month", IntlValueType::MAYBE_TEXT},
                   {"day", IntlValueType::MAYBE_TEXT},
                   {"dayPeriod", IntlValueType::MAYBE_TEXT},
                   {"hour", IntlValueType::MAYBE_TEXT},
                   {"minute", IntlValueType::MAYBE_TEXT},
                   {"second", IntlValueType::MAYBE_TEXT},
                   {"fractionalSecondDigits", IntlValueType::MAYBE_INT},
                   {"timeZoneName", IntlValueType::MAYBE_TEXT},
                   {"dateStyle", IntlValueType::MAYBE_TEXT},
                   {"timeStyle", IntlValueType::MAYBE_TEXT}},
    },
};

}

const IntlShapeInfo &IntlShapeTable::find(IntlShape shape) {
  for (const IntlShapeInfo &info : kShapes) {
    if (info.shape == shape) {
      return info;
    }
  }
  llvm_unreachable("intl shape missing from the table");
}

IntlShape IntlShapeTable::resolvedOf(IntlKind kind) {
  switch (kind) {
    case IntlKind::NumberFormat:
      return IntlShape::RESOLVED_NUMBER_FORMAT;
    case IntlKind::Collator:
      return IntlShape::RESOLVED_COLLATOR;
    case IntlKind::PluralRules:
      return IntlShape::RESOLVED_PLURAL_RULES;
    case IntlKind::RelativeTimeFormat:
      return IntlShape::RESOLVED_RELATIVE_TIME_FORMAT;
    case IntlKind::ListFormat:
      return IntlShape::RESOLVED_LIST_FORMAT;
    case IntlKind::DisplayNames:
      return IntlShape::RESOLVED_DISPLAY_NAMES;
    case IntlKind::None:
    case IntlKind::DateTimeFormat:
    case IntlKind::Segmenter:
    case IntlKind::Segments:
      return IntlShape::RESOLVED_SEGMENTER;
  }

  return IntlShape::RESOLVED_SEGMENTER;
}

llvm::StringRef IntlShapeTable::getLocaleTagField() {
  return kLocaleTagField;
}

}
