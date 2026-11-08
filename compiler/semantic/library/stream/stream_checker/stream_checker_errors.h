#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class StreamCheckerOverridesArgsErrorCode {
  NOT_CLASS_AND_LITERAL,
};

struct StreamCheckerOverridesArgsError {
  StreamCheckerOverridesArgsErrorCode code;
  std::string message;
};

enum class StreamCheckerOverriddenMethodErrorCode {
  NO_SUCH_METHOD,
};

struct StreamCheckerOverriddenMethodError {
  StreamCheckerOverriddenMethodErrorCode code;
  std::string message;
};

enum class StreamCheckerChunkNotUndefinedErrorCode {
  UNDEFINED_CHUNK,
};

struct StreamCheckerChunkNotUndefinedError {
  StreamCheckerChunkNotUndefinedErrorCode code;
  std::string message;
};

enum class StreamCheckerStreamSideErrorCode {
  NOT_READABLE,
  NOT_WRITABLE,
};

struct StreamCheckerStreamSideError {
  StreamCheckerStreamSideErrorCode code;
  std::string message;
};

class ErrorTextStreamCall {
public:
  static std::string overrides();
  static std::string noMethod(llvm::StringRef cls, llvm::StringRef method);
  static std::string undefined(llvm::StringRef method);
  static std::string chunk(llvm::StringRef method, llvm::StringRef chunk, bool onlyStrings,
                           llvm::StringRef type);
  static std::string pipe();
  static std::string noSide(llvm::StringRef type, llvm::StringRef name, bool reading);
  static std::string endWithFn();
  static std::string endArgs();
};

}
