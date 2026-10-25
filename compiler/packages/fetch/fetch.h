#pragma once

#include "compiler/packages/semver/semver.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

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

// The hash tz.lock keeps for a package: the SHA-256 of its list of files,
// one "path sha256-<hex>\n" line per file, sorted by path. `files` holds the
// paths (with '/') and the hex SHA-256 of each file's bytes.
std::string treeHash(std::vector<std::pair<std::string, std::string>> files);
// treeHash of every file under `dir`.
bool hashDirectory(llvm::StringRef dir, std::string &hash, std::string &error);
std::string sha256Hex(llvm::StringRef data);

// Says what tz downloads, on stderr: "tz: downloading jsr:@std/path 1.0.8".
void progress(const llvm::Twine &message);

// --- JSR (https://jsr.io/docs/api) ---------------------------------------

// The registry: TZ_JSR_URL, or https://jsr.io. A file:// address is a
// mirror on the disk with the same layout as the registry's API.
std::string jsrBaseUrl();

struct JsrVersions {
  struct Entry {
    Version version;
    bool yanked = false;
  };
  std::vector<Entry> versions; // as listed
};
// meta.json of @scope/name. False with an empty `error` if the registry has
// no such package.
bool jsrVersions(llvm::StringRef scope, llvm::StringRef name, JsrVersions &out,
                 std::string &error);

struct JsrVersion {
  // Every file of the version (path without the leading '/') and the hex
  // SHA-256 the registry gives for it.
  std::vector<std::pair<std::string, std::string>> files;
  // "." -> "mod.tz", "./join" -> "join.tz".
  std::vector<std::pair<std::string, std::string>> exports;
  std::string hash; // treeHash of the files
};
// <version>_meta.json, from the cache or the registry (and then kept).
bool jsrVersion(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                JsrVersion &out, std::string &error);
// The files of the version in the cache, downloaded and checked against their
// hashes if they are not there yet; `dir` is where they are.
bool jsrDownload(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                 const JsrVersion &meta, std::string &dir, std::string &error);

// --- git -------------------------------------------------------------------

// The commit `ref` (a tag, a branch or a commit; "" is the default branch)
// names in the repository at `url`. The repository is mirrored into the cache
// and, unless it was just cloned, fetched first when `refresh` is set or the
// ref is unknown.
bool gitResolve(const std::string &url, const std::string &ref, bool refresh,
                std::string &commit, std::string &error);
// The files of `commit` in the cache, checked out if they are not there yet.
bool gitCheckout(const std::string &url, const std::string &commit, std::string &dir,
                 std::string &error);

// Writes `data` to `file` (creating its directory) under another name first,
// then renames it: a tz that reads the file at the same time sees it whole.
bool writeFileAtomically(llvm::StringRef file, llvm::StringRef data, std::string &error);

// Runs a program; its stdout and stderr are collected. Returns its exit code,
// or -1 with `error` set if it could not run.
int runProgram(llvm::StringRef program, llvm::ArrayRef<std::string> args, std::string &out,
               std::string &err, std::string &error);

} // namespace tz::pkg
