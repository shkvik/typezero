#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace tz {

enum class HttpCheckerHeaderNameErrorCode {
  NOT_A_TOKEN,
};

struct HttpCheckerHeaderNameError {
  HttpCheckerHeaderNameErrorCode code;
  std::string message;
};

enum class HttpCheckerHeaderTextErrorCode {
  LINE_BREAK,
};

struct HttpCheckerHeaderTextError {
  HttpCheckerHeaderTextErrorCode code;
  std::string message;
};

enum class HttpCheckerRequestUrlErrorCode {
  INVALID_URL,
  CREDENTIALS,
};

struct HttpCheckerRequestUrlError {
  HttpCheckerRequestUrlErrorCode code;
  std::string message;
};

enum class HttpCheckerMethodNameErrorCode {
  NOT_A_TOKEN,
  FORBIDDEN,
};

struct HttpCheckerMethodNameError {
  HttpCheckerMethodNameErrorCode code;
  std::string message;
};

class ErrorTextHeader {
public:
  static std::string name(llvm::StringRef name);
  static std::string value();
};

class ErrorTextHttpResponse {
public:
  static std::string noBody(int64_t status);
  static std::string statusText();
};

class ErrorTextHttpRequest {
public:
  static std::string badUrl(llvm::StringRef text);
  static std::string userInUrl();
  static std::string unknown();
  static std::string badMethod(llvm::StringRef method);
  static std::string forbidden(llvm::StringRef method);
  static std::string getBody(llvm::StringRef method, bool given);
};

}
