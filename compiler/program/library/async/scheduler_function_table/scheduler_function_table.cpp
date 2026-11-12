#include "compiler/program/library/async/scheduler_function_table/scheduler_function_table.h"

namespace tz {

namespace {

const SchedulerFunctionInfo kRows[] = {
    {
        .name = "wait",
        .value = SchedulerFunction::WAIT,
    },
    {
        .name = "yield",
        .value = SchedulerFunction::YIELD,
    },
};

}

std::optional<SchedulerFunction> SchedulerFunctionTable::find(llvm::StringRef name) {
  for (const SchedulerFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
