#include "compiler/program/library/async/timer_method_table/timer_method_table.h"

namespace tz {

namespace {

const TimerMethodInfo kRows[] = {
    {
        .name = "ref",
        .value = TimerMethod::REF,
    },
    {
        .name = "unref",
        .value = TimerMethod::UNREF,
    },
    {
        .name = "hasRef",
        .value = TimerMethod::HAS_REF,
    },
    {
        .name = "refresh",
        .value = TimerMethod::REFRESH,
    },
};

}

std::optional<TimerMethod> TimerMethodTable::find(llvm::StringRef name) {
  for (const TimerMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
