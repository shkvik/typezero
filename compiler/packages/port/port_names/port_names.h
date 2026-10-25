#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

// "parse" -> "Parse".

std::string capitalize(llvm::StringRef s);

// A name for the default export of `file`: after the file, or after the
// package for its index.ts.
std::string derivedName(llvm::StringRef file, llvm::StringRef packageDir,
                        llvm::StringRef packageName);

} // namespace tz::pkg
