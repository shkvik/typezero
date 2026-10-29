#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ProgramEntry {
public:
  static std::string forFile(llvm::StringRef file);
  static std::string forDirectory(llvm::StringRef dir);
  static std::string rootOf(llvm::StringRef entry);
};

}
