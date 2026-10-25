#pragma once

#include "llvm/ADT/StringRef.h"
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

// Getting the sources of packages into the cache: a directory shared by every
// project on the machine (docs/packages.md, section 4). It is TZ_CACHE if
// that is set, else %LOCALAPPDATA%\tz\cache on Windows and ~/.cache/tz
// ($XDG_CACHE_HOME/tz) elsewhere. What is in it never changes once written:
//   jsr/@scope/name/<version>/...        the files of a JSR package
//   jsr/@scope/name/<version>_meta.json  its list of files with their hashes
//   git/<repository>.git                 a mirror of a git repository
//   git/<repository>/<commit>/...        the files of one commit
// A build that finds what tz.lock names there needs no network.
std::string cacheDir();
// A path in the cache.
std::string cachePath(std::initializer_list<llvm::StringRef> parts);

// The hash tz.lock keeps for a package: the SHA-256 of its list of files,
// one "path sha256-<hex>\n" line per file, sorted by path. `files` holds the
// paths (with '/') and the hex SHA-256 of each file's bytes.
std::string treeHash(std::vector<std::pair<std::string, std::string>> files);
// treeHash of every file under `dir`.
bool hashDirectory(llvm::StringRef dir, std::string &hash, std::string &error);
std::string sha256Hex(llvm::StringRef data);

// Writes `data` to `file` (creating its directory) under another name first,
// then renames it: a tz that reads the file at the same time sees it whole.
bool writeFileAtomically(llvm::StringRef file, llvm::StringRef data, std::string &error);

} // namespace tz::pkg
