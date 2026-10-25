#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include <map>
#include <string>
#include <vector>

namespace tz::pkg {

// What the package of the file being ported knows of the others.
struct PortContext {
  std::string packageName; // "ms"
  // Real path of each file -> the name of its default export, which becomes
  // a named one.
  std::map<std::string, std::string> defaultNames;
  // Packages its files import with "jsr:" -> where they come from, for its
  // tz.json.
  std::map<std::string, std::string> jsrDependencies;
};

// The changes made, by kind, for the summary.
using Counts = std::map<std::string, unsigned>;

} // namespace tz::pkg
