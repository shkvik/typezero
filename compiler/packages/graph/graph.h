#pragma once

#include "compiler/packages/package/package.h"
#include <vector>

namespace tz::pkg {

struct Graph {
  std::vector<Package> packages;
};

}
