#include "compiler/program/library/events/emitter_member_table/emitter_member_table.h"

namespace tz {

namespace {

const EmitterMemberInfo kRows[] = {
    {
        .name = "on",
        .value = EmitterMember::ON,
    },
    {
        .name = "addListener",
        .value = EmitterMember::ADD_LISTENER,
    },
    {
        .name = "prependListener",
        .value = EmitterMember::PREPEND_LISTENER,
    },
    {
        .name = "once",
        .value = EmitterMember::ONCE,
    },
    {
        .name = "prependOnceListener",
        .value = EmitterMember::PREPEND_ONCE_LISTENER,
    },
    {
        .name = "off",
        .value = EmitterMember::OFF,
    },
    {
        .name = "removeListener",
        .value = EmitterMember::REMOVE_LISTENER,
    },
    {
        .name = "emit",
        .value = EmitterMember::EMIT,
    },
    {
        .name = "listeners",
        .value = EmitterMember::LISTENERS,
    },
    {
        .name = "rawListeners",
        .value = EmitterMember::RAW_LISTENERS,
    },
    {
        .name = "listenerCount",
        .value = EmitterMember::LISTENER_COUNT,
    },
    {
        .name = "removeAllListeners",
        .value = EmitterMember::REMOVE_ALL_LISTENERS,
    },
    {
        .name = "eventNames",
        .value = EmitterMember::EVENT_NAMES,
    },
};

}

std::optional<EmitterMember> EmitterMemberTable::find(llvm::StringRef name) {
  for (const EmitterMemberInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef EmitterMemberTable::getName(EmitterMember value) {
  for (const EmitterMemberInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }

  return "";
}

bool EmitterMemberTable::isName(llvm::StringRef name, EmitterMember value) {
  llvm::StringRef expected = EmitterMemberTable::getName(value);

  return name == expected;
}

}
