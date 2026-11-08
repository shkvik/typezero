#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class UrlCheckerKnownBaseErrorCode {
  UNKNOWN_BASE,
};

struct UrlCheckerKnownBaseError {
  UrlCheckerKnownBaseErrorCode code;
  std::string message;
};

enum class UrlCheckerUrlTextErrorCode {
  INVALID_URL,
};

struct UrlCheckerUrlTextError {
  UrlCheckerUrlTextErrorCode code;
  std::string message;
};

enum class UrlCheckerKnownInputErrorCode {
  UNKNOWN_INPUT,
};

struct UrlCheckerKnownInputError {
  UrlCheckerKnownInputErrorCode code;
  std::string message;
};

enum class UrlCheckerSetterValueErrorCode {
  NOT_A_URL,
  NOT_A_PROTOCOL,
  NOT_A_PORT,
  NOT_A_HOST,
};

struct UrlCheckerSetterValueError {
  UrlCheckerSetterValueErrorCode code;
  std::string message;
};

class ErrorTextUrlNew {
public:
  static std::string base();
  static std::string invalid(llvm::StringRef text, bool based);
  static std::string input(bool based);
};

class ErrorTextUrlSetter {
public:
  static std::string href(llvm::StringRef text);
  static std::string protocol(llvm::StringRef text);
  static std::string port(llvm::StringRef text);
  static std::string host(llvm::StringRef text, bool withPort);
};

}
