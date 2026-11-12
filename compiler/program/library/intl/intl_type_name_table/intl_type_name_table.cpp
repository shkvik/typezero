#include "compiler/program/library/intl/intl_type_name_table/intl_type_name_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const IntlTypeNameInfo kRows[] = {
    {
        .name = "DateTimeFormat",
        .value = IntlTypeName::DATE_TIME_FORMAT,
    },
    {
        .name = "Segments",
        .value = IntlTypeName::SEGMENTS,
    },
    {
        .name = "SegmentData",
        .value = IntlTypeName::SEGMENT_DATA,
    },
    {
        .name = "DateTimeFormatPart",
        .value = IntlTypeName::DATE_TIME_FORMAT_PART,
    },
    {
        .name = "NumberFormatPart",
        .value = IntlTypeName::NUMBER_FORMAT_PART,
    },
    {
        .name = "RelativeTimeFormatPart",
        .value = IntlTypeName::RELATIVE_TIME_FORMAT_PART,
    },
    {
        .name = "ResolvedDateTimeFormatOptions",
        .value = IntlTypeName::RESOLVED_DATE_TIME_FORMAT_OPTIONS,
    },
    {
        .name = "DateTimeFormatOptions",
        .value = IntlTypeName::DATE_TIME_FORMAT_OPTIONS,
    },
    {
        .name = "LDMLPluralRule",
        .value = IntlTypeName::LDML_PLURAL_RULE,
    },
    {
        .name = "RelativeTimeFormatUnit",
        .value = IntlTypeName::RELATIVE_TIME_FORMAT_UNIT,
    },
    {
        .name = "UnicodeBCP47LocaleIdentifier",
        .value = IntlTypeName::UNICODE_BCP47_LOCALE_IDENTIFIER,
    },
    {
        .name = "BCP47LanguageTag",
        .value = IntlTypeName::BCP47_LANGUAGE_TAG,
    },
    {
        .name = "Locale",
        .value = IntlTypeName::LOCALE,
    },
};

}

std::optional<IntlTypeName> IntlTypeNameTable::find(llvm::StringRef name) {
  for (const IntlTypeNameInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef IntlTypeNameTable::getName(IntlTypeName value) {
  for (const IntlTypeNameInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("intl type name missing from the table");
}

}
