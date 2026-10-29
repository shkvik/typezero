#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class FileUri {
public:
  static std::string toPath(llvm::StringRef uri);
  static std::string fromPath(llvm::StringRef path);
};

}
