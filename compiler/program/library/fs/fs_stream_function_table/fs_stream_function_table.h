#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class FsStreamFunction {
  CREATE_READ_STREAM,
  CREATE_WRITE_STREAM,
};

struct FsStreamFunctionInfo {
  llvm::StringLiteral name;
  FsStreamFunction value;
};

class FsStreamFunctionTable {
public:
  static std::optional<FsStreamFunction> find(llvm::StringRef name);
};

}
