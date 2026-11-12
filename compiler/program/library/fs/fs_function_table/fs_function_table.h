#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace tz {

enum class FsFunction {
  CONSTANTS,
  STATS,
  DIRENT,
  READ_FILE_SYNC,
  WRITE_FILE_SYNC,
  APPEND_FILE_SYNC,
  EXISTS_SYNC,
  UNLINK_SYNC,
  REALPATH_SYNC,
  MKDTEMP_SYNC,
  ACCESS_SYNC,
  CHMOD_SYNC,
  TRUNCATE_SYNC,
  STAT_SYNC,
  LSTAT_SYNC,
  READDIR_SYNC,
  MKDIR_SYNC,
  RMDIR_SYNC,
  RM_SYNC,
  RENAME_SYNC,
  COPY_FILE_SYNC,
  REPLACE_SYNC,
};

struct FsFunctionInfo {
  llvm::StringLiteral name;
  FsFunction value;
};

class FsFunctionTable {
public:
  static std::optional<FsFunction> find(llvm::StringRef name);
  static std::string getSyncName(llvm::StringRef name);
};

}
