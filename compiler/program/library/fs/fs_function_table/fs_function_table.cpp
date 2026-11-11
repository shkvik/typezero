#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"

namespace tz {

namespace {

const FsFunctionInfo kRows[] = {
    {
        .name = "constants",
        .value = FsFunction::CONSTANTS,
    },
    {
        .name = "Stats",
        .value = FsFunction::STATS,
    },
    {
        .name = "Dirent",
        .value = FsFunction::DIRENT,
    },
    {
        .name = "readFileSync",
        .value = FsFunction::READ_FILE_SYNC,
    },
    {
        .name = "writeFileSync",
        .value = FsFunction::WRITE_FILE_SYNC,
    },
    {
        .name = "appendFileSync",
        .value = FsFunction::APPEND_FILE_SYNC,
    },
    {
        .name = "existsSync",
        .value = FsFunction::EXISTS_SYNC,
    },
    {
        .name = "unlinkSync",
        .value = FsFunction::UNLINK_SYNC,
    },
    {
        .name = "realpathSync",
        .value = FsFunction::REALPATH_SYNC,
    },
    {
        .name = "mkdtempSync",
        .value = FsFunction::MKDTEMP_SYNC,
    },
    {
        .name = "accessSync",
        .value = FsFunction::ACCESS_SYNC,
    },
    {
        .name = "chmodSync",
        .value = FsFunction::CHMOD_SYNC,
    },
    {
        .name = "truncateSync",
        .value = FsFunction::TRUNCATE_SYNC,
    },
    {
        .name = "statSync",
        .value = FsFunction::STAT_SYNC,
    },
    {
        .name = "lstatSync",
        .value = FsFunction::LSTAT_SYNC,
    },
    {
        .name = "readdirSync",
        .value = FsFunction::READDIR_SYNC,
    },
    {
        .name = "mkdirSync",
        .value = FsFunction::MKDIR_SYNC,
    },
    {
        .name = "rmdirSync",
        .value = FsFunction::RMDIR_SYNC,
    },
    {
        .name = "rmSync",
        .value = FsFunction::RM_SYNC,
    },
    {
        .name = "renameSync",
        .value = FsFunction::RENAME_SYNC,
    },
    {
        .name = "copyFileSync",
        .value = FsFunction::COPY_FILE_SYNC,
    },
    {
        .name = "replaceSync",
        .value = FsFunction::REPLACE_SYNC,
    },
};

}

std::optional<FsFunction> FsFunctionTable::find(llvm::StringRef name) {
  for (const FsFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
