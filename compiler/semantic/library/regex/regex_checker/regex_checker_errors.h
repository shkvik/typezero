#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz {

enum class RegexCheckerParsedFlagsErrorCode {
  INVALID_FLAGS,
};

struct RegexCheckerParsedFlagsError {
  RegexCheckerParsedFlagsErrorCode code;
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

}
