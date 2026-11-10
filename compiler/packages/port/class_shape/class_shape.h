#pragma once

#include <cstddef>
#include <string>

namespace tz::pkg {

struct ClassShape {
  size_t body = std::string::npos;
  bool extends = false;
};

}
