#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

// `dir` as the file system has it, links resolved; absolute and without dots
// if it does not exist.

std::string canonical(llvm::StringRef dir);

} // namespace tz::pkg
