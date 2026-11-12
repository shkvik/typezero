#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ChildExport {
  SYNC_FUNCTION,
  ASYNC_FUNCTION,
  FORK,
  NOT_YET,
};

struct ChildExportInfo {
  llvm::StringLiteral name;
  ChildExport kind;
};

class ChildExportTable {
public:
  static std::optional<ChildExport> find(llvm::StringRef name);
};

}
