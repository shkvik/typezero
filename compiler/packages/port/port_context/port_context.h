#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include <map>
#include <string>
#include <vector>

namespace tz::pkg {

struct PortContext {
  std::string packageName;
  std::map<std::string, std::string> defaultNames;
  std::map<std::string, std::string> jsrDependencies;
};

using Counts = std::map<std::string, unsigned>;

}
