#include "compiler/program/library/os/os_value_table/os_value_table.h"

namespace tz {

namespace {

const OsValueInfo kOsValues[] = {
    {
        .name = "EOL",
        .value = OsValue::END_OF_LINE,
    },
    {
        .name = "devNull",
        .value = OsValue::DEV_NULL,
    },
};

}

std::optional<OsValue> OsValueTable::find(llvm::StringRef name) {
  for (const OsValueInfo &info : kOsValues) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
