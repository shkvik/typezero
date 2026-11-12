#include "compiler/program/library/stream/stream_static_table/stream_static_table.h"

namespace tz {

namespace {

const StreamStaticInfo kRows[] = {
    {
        .name = "from",
        .value = StreamStatic::FROM,
    },
};

}

std::optional<StreamStatic> StreamStaticTable::find(llvm::StringRef name) {
  for (const StreamStaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
