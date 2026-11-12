#include "compiler/program/library/fs/fs_constant_table/fs_constant_table.h"

namespace tz {

namespace {

const FsConstantInfo kFsConstants[] = {
    {.name = "F_OK", .value = 0},
    {.name = "R_OK", .value = 4},
    {.name = "W_OK", .value = 2},
    {.name = "X_OK", .value = 1},
    {.name = "COPYFILE_EXCL", .value = 1},
    {.name = "COPYFILE_FICLONE", .value = 2},
    {.name = "COPYFILE_FICLONE_FORCE", .value = 4},
};

}

std::optional<int> FsConstantTable::find(llvm::StringRef name) {
  for (const FsConstantInfo &info : kFsConstants) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
