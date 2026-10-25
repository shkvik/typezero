#pragma once

#include "compiler/packages/package/package.h"
#include <vector>

namespace tz::pkg {

// The packages of a program, for the loader. packages[0] is the project.
struct Graph {
  std::vector<Package> packages;
};

} // namespace tz::pkg
