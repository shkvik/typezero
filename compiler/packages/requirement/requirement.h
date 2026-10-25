#pragma once

#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/source/source.h"
#include <string>

namespace tz::pkg {

// Something that needs a package: tz.json or another package.
struct Requirement {
  std::string by; // "tz.json", "jsr:@a/b 1.0.0"
  std::string spec;
  Source source;
};

} // namespace tz::pkg
