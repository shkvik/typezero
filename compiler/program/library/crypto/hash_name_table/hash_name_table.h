#pragma once

#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

struct HashNameInfo {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> aliases;
  bool isAnyLength;
};

class HashNameTable {
public:
  static const HashNameInfo *find(llvm::StringRef name);
  static bool isTurnedOff(llvm::StringRef name);
};

}
