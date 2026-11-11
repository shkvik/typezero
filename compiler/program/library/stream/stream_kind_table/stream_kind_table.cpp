#include "compiler/program/library/stream/stream_kind_table/stream_kind_table.h"

namespace tz {

namespace {

const StreamKindInfo kRows[] = {
    {
        .name = "Readable",
        .value = StreamKind::READABLE,
    },
    {
        .name = "Writable",
        .value = StreamKind::WRITABLE,
    },
    {
        .name = "Duplex",
        .value = StreamKind::DUPLEX,
    },
    {
        .name = "Transform",
        .value = StreamKind::TRANSFORM,
    },
    {
        .name = "PassThrough",
        .value = StreamKind::PASS_THROUGH,
    },
};

}

std::optional<StreamKind> StreamKindTable::find(llvm::StringRef name) {
  for (const StreamKindInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
