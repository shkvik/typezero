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

class ErrorTextStreamNew {
public:
  static std::string abstract(llvm::StringRef name, bool readable, bool writable);
  static std::string untyped(llvm::StringRef what, llvm::StringRef name, bool pushes);
};

class ErrorTextStreamSide {
public:
  static std::string written();
  static std::string given();
};

}
