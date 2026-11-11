#include "compiler/program/library/events/events_function_table/events_function_table.h"

namespace tz {

namespace {

const EventsFunctionInfo kRows[] = {
    {
        .name = "getEventListeners",
        .value = EventsFunction::GET_EVENT_LISTENERS,
    },
    {
        .name = "listenerCount",
        .value = EventsFunction::LISTENER_COUNT,
    },
    {
        .name = "once",
        .value = EventsFunction::ONCE,
    },
    {
        .name = "on",
        .value = EventsFunction::ON,
    },
};

}

std::optional<EventsFunction> EventsFunctionTable::find(llvm::StringRef name) {
  for (const EventsFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
