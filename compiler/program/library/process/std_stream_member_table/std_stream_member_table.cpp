#include "compiler/program/library/process/std_stream_member_table/std_stream_member_table.h"

namespace tz {

namespace {

const StdStreamMemberInfo kRows[] = {
    {
        .name = "isTTY",
        .value = StdStreamMember::IS_TTY,
    },
    {
        .name = "columns",
        .value = StdStreamMember::COLUMNS,
    },
    {
        .name = "rows",
        .value = StdStreamMember::ROWS,
    },
    {
        .name = "write",
        .value = StdStreamMember::WRITE,
    },
};

}

std::optional<StdStreamMember> StdStreamMemberTable::find(llvm::StringRef name) {
  for (const StdStreamMemberInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
