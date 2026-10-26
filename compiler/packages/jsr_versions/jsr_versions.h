#pragma once

#include "compiler/packages/version/version.h"
#include <vector>

namespace tz::pkg {

struct JsrVersions {
  struct Entry {
    Version version;
    bool yanked = false;
  };
  std::vector<Entry> versions;
};

}
