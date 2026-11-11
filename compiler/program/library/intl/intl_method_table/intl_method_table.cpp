#include "compiler/program/library/intl/intl_method_table/intl_method_table.h"

namespace tz {

namespace {

const IntlMethodInfo kIntlMethods[] = {
    {
        .name = "resolvedOptions",
        .owner = IntlKind::None,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "resolvedOptions",
        .owner = IntlKind::DateTimeFormat,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "resolvedOptions",
        .owner = IntlKind::NumberFormat,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "resolvedOptions",
        .owner = IntlKind::Collator,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "resolvedOptions",
        .owner = IntlKind::PluralRules,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "resolvedOptions",
        .owner = IntlKind::RelativeTimeFormat,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "resolvedOptions",
        .owner = IntlKind::ListFormat,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "resolvedOptions",
        .owner = IntlKind::DisplayNames,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "resolvedOptions",
        .owner = IntlKind::Segmenter,
        .method = IntlMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "format",
        .owner = IntlKind::NumberFormat,
        .method = IntlMethod::NUMBER_FORMAT,
    },
    {
        .name = "formatToParts",
        .owner = IntlKind::NumberFormat,
        .method = IntlMethod::NUMBER_FORMAT_TO_PARTS,
    },
    {
        .name = "formatRange",
        .owner = IntlKind::NumberFormat,
        .method = IntlMethod::NUMBER_FORMAT_RANGE,
    },
    {
        .name = "formatRangeToParts",
        .owner = IntlKind::NumberFormat,
        .method = IntlMethod::NUMBER_FORMAT_RANGE,
    },
    {
        .name = "compare",
        .owner = IntlKind::Collator,
        .method = IntlMethod::COLLATOR_COMPARE,
    },
    {
        .name = "select",
        .owner = IntlKind::PluralRules,
        .method = IntlMethod::PLURAL_SELECT,
    },
    {
        .name = "selectRange",
        .owner = IntlKind::PluralRules,
        .method = IntlMethod::PLURAL_SELECT_RANGE,
    },
    {
        .name = "format",
        .owner = IntlKind::RelativeTimeFormat,
        .method = IntlMethod::RELATIVE_FORMAT,
    },
    {
        .name = "formatToParts",
        .owner = IntlKind::RelativeTimeFormat,
        .method = IntlMethod::RELATIVE_FORMAT_TO_PARTS,
    },
    {
        .name = "format",
        .owner = IntlKind::ListFormat,
        .method = IntlMethod::LIST_FORMAT,
    },
    {
        .name = "formatToParts",
        .owner = IntlKind::ListFormat,
        .method = IntlMethod::LIST_FORMAT_TO_PARTS,
    },
    {
        .name = "of",
        .owner = IntlKind::DisplayNames,
        .method = IntlMethod::DISPLAY_NAME_OF,
    },
    {
        .name = "segment",
        .owner = IntlKind::Segmenter,
        .method = IntlMethod::SEGMENTER_SEGMENT,
    },
    {
        .name = "containing",
        .owner = IntlKind::Segments,
        .method = IntlMethod::SEGMENTS_CONTAINING,
    },
};

}

const IntlMethodInfo *IntlMethodTable::find(IntlKind owner, llvm::StringRef name) {
  for (const IntlMethodInfo &info : kIntlMethods) {
    if (info.owner == owner && info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
