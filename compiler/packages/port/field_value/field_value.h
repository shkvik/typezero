#pragma once

#include <cstddef>

namespace tz::pkg {

struct FieldValue {
  size_t name = 0;
  size_t declarationEnd = 0;
  size_t valueBegin = 0;
  size_t valueEnd = 0;
  bool typed = false;
  bool semicolon = false;
};

}
