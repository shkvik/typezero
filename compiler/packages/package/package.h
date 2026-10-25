#pragma once

#include "llvm/ADT/StringMap.h"
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

// A package of a program, for the loader.
struct Package {
  std::string id;   // "" for the project
  std::string name; // what its files are called after: "@std/path/join.tz"
  std::string version;
  std::string dir;          // canonical
  std::string manifestPath; // tz.json, if it has one
  bool external = false;    // from JSR or git: its files stay in `dir`
  bool jsr = false;
  std::string main;
  std::vector<std::pair<std::string, std::string>> exports;
  llvm::StringMap<unsigned> aliases; // import name -> package
  llvm::StringMap<unsigned> jsrDeps; // "@scope/name" of jsr: imports -> package
};

} // namespace tz::pkg
