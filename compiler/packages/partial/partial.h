#pragma once

#include "compiler/packages/version/version.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz::pkg {

// "1", "1.2", "1.x", "1.2.3-beta", "*": the leading components that are
// numbers; the rest are wildcards or missing.
struct Partial {
  uint64_t parts[3] = {0, 0, 0};
  unsigned count = 0;
  std::vector<std::string> pre;

  Version version() const {
    Version v;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    if (count == 3) {
      v.pre = pre;
    }

    return v;
  }
};

// Parses a version that may lack its minor or patch, or have 'x' or '*' for them.
bool parsePartial(llvm::StringRef text, Partial &out);

} // namespace tz::pkg
