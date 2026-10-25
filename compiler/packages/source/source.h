#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

// Projects and their packages (docs/packages.md): tz.json lists what a
// project depends on, tz.lock the exact versions and the hashes of their
// sources, and the sources themselves are in the cache (see package_cache.h).

// Where a package comes from, as tz.json writes it:
//   "jsr:@std/path@^1"            a JSR package and a range of its versions
//   "github:user/repo@v1.2.0"     a repository on GitHub and a tag (or a
//                                 branch or a commit; none is the default
//                                 branch)
//   "git:<address>@v1.2.0"        any git repository: what `git clone` takes
//   "file:../lib", "../lib"       a directory on the disk, used as it is
struct Source {
  enum class Kind { Jsr, Git, Local };
  Kind kind = Kind::Jsr;
  std::string scope, name; // Jsr: "std", "path"
  std::string range;       // Jsr: "^1", "" for any version
  std::string url;         // Git: what git clones
  std::string ref;         // Git: "" for the default branch
  std::string dir;         // Local: absolute and canonical

  // The package whatever its version: "jsr:@std/path", "git:<address>",
  // "file:<directory>". A program has each package in one version.
  std::string id() const;
  // A short name: "@std/path", the repository's or the directory's name.
  std::string shortName() const;
};

// Parses `spec`; a relative directory is relative to `baseDir`.
bool parseSource(llvm::StringRef spec, llvm::StringRef baseDir, Source &out, std::string &error);

// The repository's name from its address: "https://github.com/a/uuid.git" ->
// "uuid".
std::string repositoryName(llvm::StringRef url);

} // namespace tz::pkg
