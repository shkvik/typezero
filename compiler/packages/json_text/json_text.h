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

JsonProblem checkJsonText(llvm::StringRef s);

std::string jsonQuote(llvm::StringRef s);

std::pair<unsigned, unsigned> lineAndColumn(llvm::StringRef text, size_t offset);

}
