#pragma once

#include "compiler/packages/version/version.h"
#include <vector>

namespace tz::pkg {

// The versions of a JSR package, as its meta.json lists them.
struct JsrVersions {
  struct Entry {
    Version version;
    bool yanked = false;
  };
  std::vector<Entry> versions; // as listed
};

} // namespace tz::pkg
