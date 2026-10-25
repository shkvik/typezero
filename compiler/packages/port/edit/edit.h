#pragma once

#include <cstddef>
#include <string>

namespace tz::pkg {

struct Edit {
  size_t begin, end;
  std::string text;
  const char *kind; // for Counts, or null
  std::string note; // a note at the edit, for what still needs a look
};

} // namespace tz::pkg
