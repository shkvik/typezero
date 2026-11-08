#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz::pkg {

class Version {
public:
  Version() = default;
  Version(uint64_t majorNumber, uint64_t minorNumber, uint64_t patchNumber,
          std::vector<std::string> prerelease);

  std::string str() const;
  void dropPrerelease();
  bool isPrerelease() const;
  uint64_t getMajor() const;
  uint64_t getMinor() const;
  uint64_t getPatch() const;
  int compare(const Version &other) const;
  bool operator<(const Version &other) const;
  bool operator==(const Version &other) const;
  static bool parse(llvm::StringRef text, Version &out);

private:
  static int comparePrerelease(const std::string &mine, const std::string &theirs);
  static bool isNumeric(llvm::StringRef id);

  uint64_t major = 0;
  uint64_t minor = 0;
  uint64_t patch = 0;
  std::vector<std::string> pre;
};

}
