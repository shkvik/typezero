#include "compiler/program/library/date/date_legacy_method_table/date_legacy_method_table.h"

namespace tz {

namespace {

const DateLegacyMethodInfo kRows[] = {
    {
        .name = "getYear",
        .value = DateLegacyMethod::GET_YEAR,
    },
    {
        .name = "setYear",
        .value = DateLegacyMethod::SET_YEAR,
    },
    {
        .name = "toGMTString",
        .value = DateLegacyMethod::TO_GMT_STRING,
    },
};

}

std::optional<DateLegacyMethod> DateLegacyMethodTable::find(llvm::StringRef name) {
  for (const DateLegacyMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
