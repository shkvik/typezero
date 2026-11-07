#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

class Paths {
public:
  static std::string canonical(llvm::StringRef dir);
  static std::string findAbove(llvm::StringRef dir, llvm::StringRef name);
};

}
