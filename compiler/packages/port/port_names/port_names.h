#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

class PortNames {
public:
  static std::string capitalize(llvm::StringRef s);
  static std::string derivedName(llvm::StringRef file, llvm::StringRef packageDir,
                                 llvm::StringRef packageName);
};

}
