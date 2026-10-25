#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

// One version of a JSR package, as its <version>_meta.json describes it.
struct JsrVersion {
  // Every file of the version (path without the leading '/') and the hex
  // SHA-256 the registry gives for it.
  std::vector<std::pair<std::string, std::string>> files;
  // "." -> "mod.tz", "./join" -> "join.tz".
  std::vector<std::pair<std::string, std::string>> exports;
  std::string hash; // treeHash of the files
};

} // namespace tz::pkg
