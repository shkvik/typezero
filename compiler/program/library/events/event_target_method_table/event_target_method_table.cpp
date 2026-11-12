#include "compiler/program/library/events/event_target_method_table/event_target_method_table.h"

namespace tz {

namespace {

const EventTargetMethodInfo kRows[] = {
    {
        .name = "addEventListener",
        .value = EventTargetMethod::ADD_EVENT_LISTENER,
    },
    {
        .name = "removeEventListener",
        .value = EventTargetMethod::REMOVE_EVENT_LISTENER,
    },
};

}

std::optional<EventTargetMethod> EventTargetMethodTable::find(llvm::StringRef name) {
  for (const EventTargetMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
