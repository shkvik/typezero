#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

class TextFile {
public:
  static bool readText(llvm::StringRef file, std::string &text);
};

}
