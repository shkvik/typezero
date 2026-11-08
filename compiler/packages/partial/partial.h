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
    std::vector<std::string> prerelease = this->count == 3 ? this->pre : std::vector<std::string>();

    return Version(this->parts[0], this->parts[1], this->parts[2], prerelease);
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
