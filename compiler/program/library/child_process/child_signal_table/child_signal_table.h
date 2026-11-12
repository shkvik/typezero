#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

struct ChildSignalInfo {
  llvm::StringLiteral name;
  bool isOnWindows;
  bool isOnPosix;
};

class ChildSignalTable {
public:
  static bool isKnown(llvm::StringRef name, bool isWindows);
};

}
