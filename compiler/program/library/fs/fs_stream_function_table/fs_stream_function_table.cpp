#include "compiler/program/library/fs/fs_stream_function_table/fs_stream_function_table.h"

namespace tz {

namespace {

const FsStreamFunctionInfo kRows[] = {
    {
        .name = "createReadStream",
        .value = FsStreamFunction::CREATE_READ_STREAM,
    },
    {
        .name = "createWriteStream",
        .value = FsStreamFunction::CREATE_WRITE_STREAM,
    },
};

}

std::optional<FsStreamFunction> FsStreamFunctionTable::find(llvm::StringRef name) {
  for (const FsStreamFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
