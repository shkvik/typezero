#pragma once

#include "llvm/ADT/StringRef.h"
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

std::string cacheDir();
std::string cachePath(std::initializer_list<llvm::StringRef> parts);

std::string treeHash(std::vector<std::pair<std::string, std::string>> files);
bool hashDirectory(llvm::StringRef dir, std::string &hash, std::string &error);
std::string sha256Hex(llvm::StringRef data);

bool writeFileAtomically(llvm::StringRef file, llvm::StringRef data, std::string &error);

}
