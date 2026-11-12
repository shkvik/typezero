#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class BufferExport {
  BUFFER,
  NOT_YET,
};

struct BufferExportInfo {
  llvm::StringLiteral name;
  BufferExport kind;
};

class BufferExportTable {
public:
  static std::optional<BufferExport> find(llvm::StringRef name);
};

}
