#pragma once

#include "compiler/packages/dependency/dependency.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

struct Manifest {
  std::string path;
  std::string dir;
  std::string text;
  bool isTzJson = false;
  std::string name, version, main;
  size_t mainOffset = 0;
  std::vector<std::pair<std::string, std::string>> exports;
  std::vector<Dependency> dependencies;

  size_t depsBegin = std::string::npos, depsEnd = 0;
  size_t lastMemberEnd = std::string::npos, rootBegin = 0, rootEnd = 0;
  std::string indent = "  ";
};

std::string findManifest(llvm::StringRef dir);
bool readManifest(llvm::StringRef file, Manifest &out);
bool readManifestText(llvm::StringRef file, std::string text, Manifest &out);
bool readPackageDir(llvm::StringRef dir, Manifest &out);
std::string withDependencies(const Manifest &manifest, std::vector<Dependency> deps);

}
