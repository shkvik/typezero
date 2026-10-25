#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz::pkg {

struct Dependency {
  std::string alias; // the name imports use
  std::string spec;  // where the package comes from, as written
  size_t offset = 0; // of the spec in tz.json, for errors
};

// "@scope/name" and "name" are package names that can be imported; the error
// says why another is not.
bool checkAlias(llvm::StringRef alias, std::string &error);

} // namespace tz::pkg
