#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>
#include <utility>

namespace tz::pkg {

struct JsonProblem {
  std::string message;
  size_t offset = 0;
};

class JsonText {
public:
  static std::string jsonQuote(llvm::StringRef s);
  static JsonProblem checkJsonText(llvm::StringRef s);
  static std::pair<unsigned, unsigned> lineAndColumn(llvm::StringRef text, size_t offset);
};

}
