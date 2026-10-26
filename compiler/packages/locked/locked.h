#pragma once

#include <string>
#include <vector>

namespace tz::pkg {

struct Locked {
  std::string id;
  std::string version;
  std::string ref, commit;
  std::string hash;
  std::vector<std::string> dependencies;
  std::string describe() const;
};

}
