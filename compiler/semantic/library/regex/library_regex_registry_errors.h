#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace tz {

enum class LibraryRegexRegistryGroupIndexErrorCode {
  NO_SUCH_GROUP,
};

struct LibraryRegexRegistryGroupIndexError {
  LibraryRegexRegistryGroupIndexErrorCode code;
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

enum class LibraryRegexRegistryParsedFlagsErrorCode {
  INVALID_FLAGS,
};

struct LibraryRegexRegistryParsedFlagsError {
  LibraryRegexRegistryParsedFlagsErrorCode code;
  std::string message;
};

class ErrorTextRegexFlags {
public:
  static std::string invalid(llvm::StringRef flags);
};

class ErrorTextRegexLit {
public:
  static std::string invalid(llvm::StringRef error);
};

class ErrorTextRegexMethod {
public:
  static std::string testG();
  static std::string compile();
  static std::string noMember(llvm::StringRef name);
  static std::string notCalled(llvm::StringRef name, bool method);
};

class ErrorTextRegExpNew {
public:
  static std::string arity(size_t count);
  static std::string unknown(bool flags);
};

class ErrorTextRegExpStatic {
public:
  static std::string escape();
  static std::string legacy(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name);
};

enum class LibraryRegexRegistryReplacementGroupsErrorCode {
  NO_SUCH_GROUP,
  NO_SUCH_NAMED_GROUP,
};

struct LibraryRegexRegistryReplacementGroupsError {
  LibraryRegexRegistryReplacementGroupsErrorCode code;
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
