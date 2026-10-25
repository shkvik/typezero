#pragma once

#include <string>
#include <vector>

namespace tz::pkg {

// One package of tz.lock: the version a program uses and the hash of its
// sources.
struct Locked {
  std::string id;
  std::string version;     // JSR
  std::string ref, commit; // git
  std::string hash;
  // What it depends on, as its tz.json or its imports write it.
  std::vector<std::string> dependencies;
  std::string describe() const; // "jsr:@std/path 1.0.8"
};

} // namespace tz::pkg
