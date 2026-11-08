#pragma once

#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

class FsErrorTypes {
public:
  static llvm::StringRef verbOf(llvm::StringRef name);
  static std::vector<llvm::StringRef> getTypeNames();
};

}
