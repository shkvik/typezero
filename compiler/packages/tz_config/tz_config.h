#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

struct TzConfig {
  std::string path;
  std::string dir;
  std::string outDir;
};

std::string findTzConfig(llvm::StringRef dir);
bool readTzConfig(llvm::StringRef file, TzConfig &out);

}
