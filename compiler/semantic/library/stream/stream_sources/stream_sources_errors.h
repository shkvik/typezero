#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class StreamSourcesValuesIteratorErrorCode {
  NOT_ITERABLE,
};

struct StreamSourcesValuesIteratorError {
  StreamSourcesValuesIteratorErrorCode code;
  std::string message;
};

class ErrorTextStreamFrom {
public:
  static std::string source(llvm::StringRef type);
};

}
