#include "compiler/program/library/async/timers_promises_function_table/timers_promises_function_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const TimersPromisesFunctionInfo kRows[] = {
    {
        .name = "setTimeout",
        .value = TimersPromisesFunction::SET_TIMEOUT,
    },
    {
        .name = "setImmediate",
        .value = TimersPromisesFunction::SET_IMMEDIATE,
    },
    {
        .name = "setInterval",
        .value = TimersPromisesFunction::SET_INTERVAL,
    },
    {
        .name = "scheduler",
        .value = TimersPromisesFunction::SCHEDULER,
    },
};

}

std::optional<TimersPromisesFunction> TimersPromisesFunctionTable::find(llvm::StringRef name) {
  for (const TimersPromisesFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef TimersPromisesFunctionTable::getName(TimersPromisesFunction value) {
  for (const TimersPromisesFunctionInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("timers promises function missing from the table");
}

}
