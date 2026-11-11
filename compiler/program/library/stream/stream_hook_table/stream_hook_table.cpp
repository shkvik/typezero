#include "compiler/program/library/stream/stream_hook_table/stream_hook_table.h"

namespace tz {

namespace {

const StreamHookInfo kRows[] = {
    {
        .name = "onRead",
        .value = StreamHook::ON_READ,
    },
    {
        .name = "onWrite",
        .value = StreamHook::ON_WRITE,
    },
    {
        .name = "onWritev",
        .value = StreamHook::ON_WRITEV,
    },
    {
        .name = "onTransform",
        .value = StreamHook::ON_TRANSFORM,
    },
    {
        .name = "onFlush",
        .value = StreamHook::ON_FLUSH,
    },
    {
        .name = "onFinal",
        .value = StreamHook::ON_FINAL,
    },
    {
        .name = "onDestroy",
        .value = StreamHook::ON_DESTROY,
    },
    {
        .name = "onConstruct",
        .value = StreamHook::ON_CONSTRUCT,
    },
};

}

std::optional<StreamHook> StreamHookTable::find(llvm::StringRef name) {
  for (const StreamHookInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
