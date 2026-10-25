#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>
#include <utility>

namespace tz::pkg {

// `s` as a JSON string literal, quotes included.
std::string jsonQuote(llvm::StringRef s);

// The line and column (both from 1) of `offset` in `text`.
std::pair<unsigned, unsigned> lineAndColumn(llvm::StringRef text, size_t offset);

} // namespace tz::pkg
