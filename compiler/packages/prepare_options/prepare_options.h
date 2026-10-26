#pragma once

#include <string>
#include <vector>

namespace tz::pkg {

struct PrepareOptions {
  bool update = false;
  std::vector<std::string> only;
};

}
