#include "compiler/program/library/stream/stream_method_table/stream_method_table.h"

namespace tz {

namespace {

const StreamMethodInfo kRows[] = {
    {
        .name = "push",
        .value = StreamMethod::PUSH,
    },
    {
        .name = "unshift",
        .value = StreamMethod::UNSHIFT,
    },
    {
        .name = "write",
        .value = StreamMethod::WRITE,
    },
    {
        .name = "end",
        .value = StreamMethod::END,
    },
    {
        .name = "setEncoding",
        .value = StreamMethod::SET_ENCODING,
    },
    {
        .name = "pipe",
        .value = StreamMethod::PIPE,
    },
};

}

std::optional<StreamMethod> StreamMethodTable::find(llvm::StringRef name) {
  for (const StreamMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
