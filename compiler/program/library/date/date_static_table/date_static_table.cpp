#include "compiler/program/library/date/date_static_table/date_static_table.h"

namespace tz {

namespace {

const DateStaticInfo kRows[] = {
    {
        .name = "now",
        .value = DateStatic::NOW,
    },
    {
        .name = "UTC",
        .value = DateStatic::UTC,
    },
    {
        .name = "parse",
        .value = DateStatic::PARSE,
    },
};

}

std::optional<DateStatic> DateStaticTable::find(llvm::StringRef name) {
  for (const DateStaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
