#pragma once

#include <cstddef>
#include <string>

namespace tz::pkg {

struct ImportRef {
  std::string specifier;
  size_t offset;
};

}
