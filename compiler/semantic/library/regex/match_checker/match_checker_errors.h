#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace tz {

enum class MatchCheckerGroupIndexErrorCode {
  NO_SUCH_GROUP,
};

struct MatchCheckerGroupIndexError {
  MatchCheckerGroupIndexErrorCode code;
  std::string message;
};

class ErrorTextUnknownRegex {
public:
  static std::string text();
};

class ErrorTextMatch {
public:
  static std::string groups(bool known);
  static std::string indices(bool known);
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string noGroup(int groups, int64_t index);
};

}
