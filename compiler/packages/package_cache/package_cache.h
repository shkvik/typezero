#pragma once

#include "llvm/ADT/StringRef.h"
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

class PackageCache {
public:
  static std::string cacheDir();
  static std::string sha256Hex(llvm::StringRef data);
  static std::string cachePath(std::initializer_list<llvm::StringRef> parts);
  static std::string treeHash(std::vector<std::pair<std::string, std::string>> files);
  static bool hashDirectory(llvm::StringRef dir, std::string &hash, std::string &error);
  static bool writeFileAtomically(llvm::StringRef file, llvm::StringRef data, std::string &error);
};

}
