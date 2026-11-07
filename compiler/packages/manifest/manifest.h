#pragma once

#include "compiler/packages/dependency/dependency.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

struct JsonValue;

struct Manifest {
  std::string path;
  std::string dir;
  std::string text;
  bool isPackageJson = false;
  std::string name, version, main;
  size_t mainOffset = 0;
  std::vector<std::pair<std::string, std::string>> exports;
  std::vector<Dependency> dependencies;
  std::vector<std::pair<std::string, std::string>> scripts;

  size_t depsBegin = std::string::npos, depsEnd = 0;
  size_t lastMemberEnd = std::string::npos, rootBegin = 0, rootEnd = 0;
  std::string indent = "  ";
};

class ManifestFile {
public:
  static std::string findManifest(llvm::StringRef dir);
  static bool namesTzFiles(llvm::StringRef packageJson);
  static bool readManifest(llvm::StringRef file, Manifest &out);
  static bool readPackageDir(llvm::StringRef dir, Manifest &out);
  static bool readManifestText(llvm::StringRef file, std::string text, Manifest &out);
  static std::string withDependencies(const Manifest &manifest, std::vector<Dependency> deps);

private:
  static bool readExports(const Manifest &out, const JsonValue &value,
                          std::vector<std::pair<std::string, std::string>> &exports);
  static bool parseManifest(Manifest &out);
};

}
