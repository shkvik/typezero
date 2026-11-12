#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <optional>
#include <vector>

namespace tz {

enum class FsClass {
  STATS,
  DIRENT,
};

enum class FsFieldType {
  INT,
  DOUBLE,
  STRING,
};

struct FsFieldInfo {
  llvm::StringLiteral name;
  FsFieldType type;
};

struct FsClassInfo {
  FsClass fsClass;
  llvm::StringLiteral name;
  std::vector<FsFieldInfo> fields;
  size_t privateFrom;
};

struct FileKindInfo {
  llvm::StringLiteral method;
  int fileKind;
};

class FsClassShape {
public:
  static const FsClassInfo &find(FsClass fsClass);
  static bool isStatsDate(llvm::StringRef name);
  static std::optional<int> fileKindOf(llvm::StringRef method);
};

}
