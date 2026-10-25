#pragma once

#include "compiler/packages/dependency/dependency.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

// tz.json: a project, or a package from git or a directory. A package with
// no tz.json says what it can of itself in jsr.json or deno.json.
struct Manifest {
  std::string path; // the file read, "" if there is none
  std::string dir;  // the package's directory, canonical
  std::string text;
  bool isTzJson = false;
  std::string name, version, main;
  size_t mainOffset = 0;
  // "." -> "mod.tz", "./mini" -> "mini.tz": what imports of the package can
  // name; empty when the package does not say.
  std::vector<std::pair<std::string, std::string>> exports;
  std::vector<Dependency> dependencies;

  // For edits of "dependencies": where its value is ([depsBegin, depsEnd);
  // npos if there is none), where the last member ends, and how members are
  // indented.
  size_t depsBegin = std::string::npos, depsEnd = 0;
  size_t lastMemberEnd = std::string::npos, rootBegin = 0, rootEnd = 0;
  std::string indent = "  ";
};

// The tz.json in `dir` or in the nearest directory above it; "" if none.
std::string findManifest(llvm::StringRef dir);
// Reads a tz.json, reporting its errors.
bool readManifest(llvm::StringRef file, Manifest &out);
// The same with the text given: tz.json as an edit would leave it.
bool readManifestText(llvm::StringRef file, std::string text, Manifest &out);
// What the package in `dir` says of itself: tz.json, else jsr.json or
// deno.json, else nothing.
bool readPackageDir(llvm::StringRef dir, Manifest &out);
// The text of tz.json with `deps` as its dependencies (sorted by name).
std::string withDependencies(const Manifest &manifest, std::vector<Dependency> deps);

} // namespace tz::pkg
