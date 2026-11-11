#include "compiler/program/library/stream/stream_function_table/stream_function_table.h"

namespace tz {

namespace {

const StreamFunctionInfo kRows[] = {
    {
        .name = "pipeline",
        .value = StreamFunction::PIPELINE,
    },
    {
        .name = "finished",
        .value = StreamFunction::FINISHED,
    },
    {
        .name = "compose",
        .value = StreamFunction::COMPOSE,
    },
};

}

std::optional<StreamFunction> StreamFunctionTable::find(llvm::StringRef name) {
  for (const StreamFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
