#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz::pkg {

struct Dependency {
  std::string alias;
  std::string spec;
  size_t offset = 0;
};

bool checkAlias(llvm::StringRef alias, std::string &error);

}
