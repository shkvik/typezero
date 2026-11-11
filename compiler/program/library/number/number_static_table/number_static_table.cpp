#include "compiler/program/library/number/number_static_table/number_static_table.h"

namespace tz {

namespace {

const NumberStaticInfo kRows[] = {
    {
        .name = "parseInt",
        .value = NumberStatic::PARSE_INT,
    },
    {
        .name = "parseFloat",
        .value = NumberStatic::PARSE_FLOAT,
    },
    {
        .name = "isInteger",
        .value = NumberStatic::IS_INTEGER,
    },
    {
        .name = "isSafeInteger",
        .value = NumberStatic::IS_SAFE_INTEGER,
    },
};

}

std::optional<NumberStatic> NumberStaticTable::find(llvm::StringRef name) {
  for (const NumberStaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
