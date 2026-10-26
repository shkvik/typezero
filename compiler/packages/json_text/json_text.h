#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>
#include <utility>

namespace tz::pkg {

std::string jsonQuote(llvm::StringRef s);

std::pair<unsigned, unsigned> lineAndColumn(llvm::StringRef text, size_t offset);

}
