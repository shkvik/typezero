#pragma once

#include <cstddef>
#include <string>

namespace tz::pkg {

struct Edit {
  size_t begin, end;
  std::string text;
  const char *kind;
  std::string note;
};

}
