#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tz::pkg {

struct NativeEntry {
  std::string name;
  std::string license;
  std::string licenseFile;
  std::vector<std::string> sources;
  std::vector<std::string> libraries;
  std::vector<std::string> defines;
  std::vector<std::string> includes;
  std::vector<std::string> systemLibraries;
  std::vector<std::string> dlls;
  std::vector<std::string> os;
  std::vector<std::string> cpu;
  std::vector<std::string> debugDefines;
  std::string dir;
  std::string manifestPath;
  size_t offset = 0;
};

}
