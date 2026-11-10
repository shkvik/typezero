#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class StreamNewCheckerConcreteStreamErrorCode {
  ABSTRACT_STREAM,
};

struct StreamNewCheckerConcreteStreamError {
  StreamNewCheckerConcreteStreamErrorCode code;
  std::string message;
};

enum class StreamNewCheckerChunksGivenErrorCode {
  CHUNK_TYPE_MISSING,
};

struct StreamNewCheckerChunksGivenError {
  StreamNewCheckerChunksGivenErrorCode code;
  std::string message;
};

class ErrorTextStreamNew {
public:
  static std::string abstract(llvm::StringRef name, bool readable, bool writable);
  static std::string untyped(llvm::StringRef name);
};

}
