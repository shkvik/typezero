#pragma once

#include "compiler/packages/version/version.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz::pkg {

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

class PartialParser {
public:
  static bool parsePartial(llvm::StringRef text, Partial &out);

private:
  static bool parseNumber(llvm::StringRef text, uint64_t &out);
  static bool parsePre(llvm::StringRef text, std::vector<std::string> &out);
};

}
