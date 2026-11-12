#include "compiler/program/library/date/date_format_method_table/date_format_method_table.h"

namespace tz {

namespace {

const DateFormatMethodInfo kRows[] = {
    {
        .name = "format",
        .value = DateFormatMethod::FORMAT,
    },
    {
        .name = "formatToParts",
        .value = DateFormatMethod::FORMAT_TO_PARTS,
    },
    {
        .name = "resolvedOptions",
        .value = DateFormatMethod::RESOLVED_OPTIONS,
    },
    {
        .name = "formatRange",
        .value = DateFormatMethod::FORMAT_RANGE,
    },
    {
        .name = "formatRangeToParts",
        .value = DateFormatMethod::FORMAT_RANGE_TO_PARTS,
    },
};

}

std::optional<DateFormatMethod> DateFormatMethodTable::find(llvm::StringRef name) {
  for (const DateFormatMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
