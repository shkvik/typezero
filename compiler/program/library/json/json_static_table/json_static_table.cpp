#include "compiler/program/library/json/json_static_table/json_static_table.h"

namespace tz {

namespace {

const JsonStaticInfo kRows[] = {
    {
        .name = "parse",
        .value = JsonStatic::PARSE,
    },
    {
        .name = "stringify",
        .value = JsonStatic::STRINGIFY,
    },
};

}

std::optional<JsonStatic> JsonStaticTable::find(llvm::StringRef name) {
  for (const JsonStaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
