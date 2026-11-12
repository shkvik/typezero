#include "compiler/program/library/regex/regex_static_table/regex_static_table.h"

namespace tz {

namespace {

const RegexStaticInfo kRows[] = {
    {
        .name = "parse",
        .value = RegexStatic::PARSE,
    },
    {
        .name = "escape",
        .value = RegexStatic::ESCAPE,
    },
};

}

std::optional<RegexStatic> RegexStaticTable::find(llvm::StringRef name) {
  for (const RegexStaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
