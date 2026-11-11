#include "compiler/program/library/fs/readline_method_table/readline_method_table.h"

namespace tz {

namespace {

const ReadlineMethodInfo kRows[] = {
    {
        .name = "close",
        .value = ReadlineMethod::CLOSE,
    },
    {
        .name = "question",
        .value = ReadlineMethod::QUESTION,
    },
    {
        .name = "on",
        .value = ReadlineMethod::ON,
    },
    {
        .name = "once",
        .value = ReadlineMethod::ONCE,
    },
    {
        .name = "addListener",
        .value = ReadlineMethod::ADD_LISTENER,
    },
    {
        .name = "off",
        .value = ReadlineMethod::OFF,
    },
    {
        .name = "removeListener",
        .value = ReadlineMethod::REMOVE_LISTENER,
    },
    {
        .name = "removeAllListeners",
        .value = ReadlineMethod::REMOVE_ALL_LISTENERS,
    },
    {
        .name = "emit",
        .value = ReadlineMethod::EMIT,
    },
    {
        .name = "prependListener",
        .value = ReadlineMethod::PREPEND_LISTENER,
    },
    {
        .name = "prependOnceListener",
        .value = ReadlineMethod::PREPEND_ONCE_LISTENER,
    },
    {
        .name = "listeners",
        .value = ReadlineMethod::LISTENERS,
    },
    {
        .name = "listenerCount",
        .value = ReadlineMethod::LISTENER_COUNT,
    },
    {
        .name = "addEventListener",
        .value = ReadlineMethod::ADD_EVENT_LISTENER,
    },
    {
        .name = "removeEventListener",
        .value = ReadlineMethod::REMOVE_EVENT_LISTENER,
    },
    {
        .name = "dispatchEvent",
        .value = ReadlineMethod::DISPATCH_EVENT,
    },
};

}

std::optional<ReadlineMethod> ReadlineMethodTable::find(llvm::StringRef name) {
  for (const ReadlineMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
