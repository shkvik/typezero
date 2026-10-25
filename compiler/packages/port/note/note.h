#pragma once

#include <cstddef>
#include <string>

namespace tz::pkg {

struct Note {
  size_t offset;
  std::string message;
};

} // namespace tz::pkg
