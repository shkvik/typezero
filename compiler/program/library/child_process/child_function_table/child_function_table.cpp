#include "compiler/program/library/child_process/child_function_table/child_function_table.h"

namespace tz {

namespace {

const ChildFunctionInfo kRows[] = {
    {
        .name = "spawnSync",
        .value = ChildFunction::SPAWN_SYNC,
    },
    {
        .name = "execFileSync",
        .value = ChildFunction::EXEC_FILE_SYNC,
    },
    {
        .name = "execSync",
        .value = ChildFunction::EXEC_SYNC,
    },
};

}

std::optional<ChildFunction> ChildFunctionTable::find(llvm::StringRef name) {
  for (const ChildFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
