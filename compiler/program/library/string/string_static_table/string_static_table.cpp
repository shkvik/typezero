#include "compiler/program/library/string/string_static_table/string_static_table.h"

namespace tz {

namespace {

const StringStaticInfo kRows[] = {
    {
        .name = "raw",
        .value = StringStatic::RAW,
    },
    {
        .name = "fromCharCode",
        .value = StringStatic::FROM_CHAR_CODE,
    },
    {
        .name = "fromCodePoint",
        .value = StringStatic::FROM_CODE_POINT,
    },
};

}

std::optional<StringStatic> StringStaticTable::find(llvm::StringRef name) {
  for (const StringStaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
