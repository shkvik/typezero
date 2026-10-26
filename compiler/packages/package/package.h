#pragma once

#include "llvm/ADT/StringMap.h"
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

struct Package {
  std::string id;
  std::string name;
  std::string version;
  std::string dir;
  std::string manifestPath;
  bool external = false;
  bool jsr = false;
  std::string main;
  std::vector<std::pair<std::string, std::string>> exports;
  llvm::StringMap<unsigned> aliases;
  llvm::StringMap<unsigned> jsrDeps;
};

}
