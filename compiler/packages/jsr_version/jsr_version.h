#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

struct JsrVersion {
  std::vector<std::pair<std::string, std::string>> files;
  std::vector<std::pair<std::string, std::string>> exports;
  std::string hash;
};

}
