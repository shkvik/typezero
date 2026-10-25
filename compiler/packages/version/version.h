#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz::pkg {

// A semantic version, "1.2.3" or "1.2.3-beta.2" (build metadata after '+' is
// ignored, as semver says), compared by semver's precedence.
struct Version {
  uint64_t major = 0, minor = 0, patch = 0;
  std::vector<std::string> pre; // "beta", "2"

  static bool parse(llvm::StringRef text, Version &out);
  bool isPrerelease() const { return !pre.empty(); }
  int compare(const Version &other) const;
  bool operator<(const Version &other) const { return compare(other) < 0; }
  bool operator==(const Version &other) const { return compare(other) == 0; }
  std::string str() const;
};

} // namespace tz::pkg
