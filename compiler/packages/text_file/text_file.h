#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

// The text of `file`, reporting why it cannot be read.

bool readText(llvm::StringRef file, std::string &text);

} // namespace tz::pkg
