#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class OsShapeField {
  CPU_USER,
  CPU_NICE,
  CPU_SYS,
  CPU_IDLE,
  CPU_IRQ,
  CPU_MODEL,
  CPU_SPEED,
  CPU_TIMES,
  USER_ID,
  GROUP_ID,
  USER_NAME,
  HOME_DIRECTORY,
  SHELL,
};

struct OsShapeFieldInfo {
  llvm::StringLiteral name;
  OsShapeField value;
};

class OsShapeFieldTable {
public:
  static std::optional<OsShapeField> find(llvm::StringRef name);
  static llvm::StringRef getName(OsShapeField value);
};

}
