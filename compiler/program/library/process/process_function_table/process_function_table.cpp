#include "compiler/program/library/process/process_function_table/process_function_table.h"

namespace tz {

namespace {

const ProcessFunctionInfo kRows[] = {
    {
        .name = "processOn",
        .value = ProcessFunction::ON,
    },
    {
        .name = "processOnAsync",
        .value = ProcessFunction::ON_ASYNC,
    },
    {
        .name = "processOff",
        .value = ProcessFunction::OFF,
    },
    {
        .name = "processOffAsync",
        .value = ProcessFunction::OFF_ASYNC,
    },
    {
        .name = "processStdin",
        .value = ProcessFunction::STDIN,
    },
    {
        .name = "processStdout",
        .value = ProcessFunction::STDOUT,
    },
};

}

llvm::StringRef ProcessFunctionTable::getName(ProcessFunction value) {
  for (const ProcessFunctionInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }

  return "";
}

}
