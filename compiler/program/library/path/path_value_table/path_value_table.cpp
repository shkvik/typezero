#include "compiler/program/library/path/path_value_table/path_value_table.h"

namespace tz {

namespace {

const PathValueInfo kPathValues[] = {
    {
        .name = "sep",
        .value = PathValue::SEPARATOR,
    },
    {
        .name = "delimiter",
        .value = PathValue::DELIMITER,
    },
    {
        .name = "posix",
        .value = PathValue::POSIX_MODULE,
    },
    {
        .name = "win32",
        .value = PathValue::WIN32_MODULE,
    },
};

}

std::optional<PathValue> PathValueTable::find(llvm::StringRef name) {
  for (const PathValueInfo &info : kPathValues) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
