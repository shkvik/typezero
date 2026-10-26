#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz::pkg {

struct Version {
  uint64_t major = 0, minor = 0, patch = 0;
  std::vector<std::string> pre;

  static bool parse(llvm::StringRef text, Version &out);
  bool isPrerelease() const { return !pre.empty(); }
  int compare(const Version &other) const;
  bool operator<(const Version &other) const { return compare(other) < 0; }
  bool operator==(const Version &other) const { return compare(other) == 0; }
  std::string str() const;
};

}
