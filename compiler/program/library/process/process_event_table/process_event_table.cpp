#include "compiler/program/library/process/process_event_table/process_event_table.h"

namespace tz {

namespace {

const ProcessEventInfo kRows[] = {
    {
        .name = "uncaughtException",
        .value = ProcessEvent::UNCAUGHT_EXCEPTION,
        .kind = ProcessEventKind::THROWN,
    },
    {
        .name = "unhandledRejection",
        .value = ProcessEvent::UNHANDLED_REJECTION,
        .kind = ProcessEventKind::THROWN,
    },
    {
        .name = "SIGINT",
        .value = ProcessEvent::SIGINT,
        .kind = ProcessEventKind::SIGNAL,
    },
    {
        .name = "SIGTERM",
        .value = ProcessEvent::SIGTERM,
        .kind = ProcessEventKind::SIGNAL,
    },
    {
        .name = "SIGHUP",
        .value = ProcessEvent::SIGHUP,
        .kind = ProcessEventKind::SIGNAL,
    },
    {
        .name = "SIGBREAK",
        .value = ProcessEvent::SIGBREAK,
        .kind = ProcessEventKind::SIGNAL,
    },
};

}

const ProcessEventInfo *ProcessEventTable::find(llvm::StringRef name) {
  for (const ProcessEventInfo &info : kRows) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
