#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace tz {

enum class StringRegexCheckerReplacementGroupsErrorCode {
  NO_SUCH_GROUP,
  NO_SUCH_NAMED_GROUP,
};

struct StringRegexCheckerReplacementGroupsError {
  StringRegexCheckerReplacementGroupsErrorCode code;
  std::string message;
};

class ErrorTextStringRegex {
public:
  static std::string matchFlag();
  static std::string needsG(llvm::StringRef name, llvm::StringRef pattern, llvm::StringRef flags);
  static std::string escapedRe(llvm::StringRef text);
  static std::string functionRe(llvm::StringRef escaped, llvm::StringRef text);
  static std::string replaceStr(llvm::StringRef text, llvm::StringRef regex);
  static std::string searchStr(llvm::StringRef text, llvm::StringRef regex, bool literal);
  static std::string includeStr(llvm::StringRef name, llvm::StringRef text, llvm::StringRef regex,
                                bool literal);
  static std::string notRegex(llvm::StringRef name, bool orString, llvm::StringRef type);
  static std::string noGroup(uint32_t groups, uint32_t one);
  static std::string noNamed(bool named, llvm::StringRef group);
  static std::string onlyMatch();
  static std::string passes(llvm::StringRef name, size_t count, int groups, bool namedGroups);
};

}
