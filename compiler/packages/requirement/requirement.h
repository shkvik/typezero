#pragma once

#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/source/source.h"
#include <string>

namespace tz::pkg {

struct Requirement {
  std::string by;
  std::string spec;
  Source source;
};

}
