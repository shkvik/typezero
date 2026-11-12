#include "compiler/program/library/events/emitter_event_table/emitter_event_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const EmitterEventInfo kRows[] = {
    {
        .name = "error",
        .value = EmitterEvent::ERROR_EVENT,
    },
    {
        .name = "newListener",
        .value = EmitterEvent::NEW_LISTENER,
    },
    {
        .name = "removeListener",
        .value = EmitterEvent::REMOVE_LISTENER,
    },
};

}

std::optional<EmitterEvent> EmitterEventTable::find(llvm::StringRef name) {
  for (const EmitterEventInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef EmitterEventTable::getName(EmitterEvent value) {
  for (const EmitterEventInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("emitter event missing from the table");
}

}
