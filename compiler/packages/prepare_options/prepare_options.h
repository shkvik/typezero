#pragma once

#include <string>
#include <vector>

namespace tz::pkg {

struct PrepareOptions {
  // tz update: take the newest versions tz.json allows, of the packages in
  // `only` (ids), or of all if it is empty.
  bool update = false;
  std::vector<std::string> only;
};

} // namespace tz::pkg
