#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

// Projects and their packages (docs/packages.md): tz.json lists what a
// project depends on, tz.lock the exact versions and the hashes of their
// sources, and the sources themselves are in the cache (see fetch.h).

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

// "@scope/name" or "name", as an import names a package; the rest of the
// specifier is the path inside it ("./mini" for "zod/mini", "." for "zod").
bool splitPackageSpecifier(llvm::StringRef specifier, std::string &name, std::string &subpath);

// An import "jsr:@std/path@^1/join", as JSR writes the imports of the
// packages it publishes: `package` "@std/path", `source` "jsr:@std/path@^1"
// (what tz.lock records), `subpath` "/join" ("" for none). False if it is not
// one.
bool splitJsrImport(llvm::StringRef specifier, std::string &package, std::string &source,
                    std::string &subpath);

struct Dependency {
  std::string alias; // the name imports use
  std::string spec;  // where the package comes from, as written
  size_t offset = 0; // of the spec in tz.json, for errors
};

// tz.json: a project, or a package from git or a directory. A package with
// no tz.json says what it can of itself in jsr.json or deno.json.
struct Manifest {
  std::string path; // the file read, "" if there is none
  std::string dir;  // the package's directory, canonical
  std::string text;
  bool isTzJson = false;
  std::string name, version, main;
  size_t mainOffset = 0;
  // "." -> "mod.ts", "./mini" -> "mini.ts": what imports of the package can
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
// "@scope/name" and "name" are package names that can be imported; the error
// says why another is not.
bool checkAlias(llvm::StringRef alias, std::string &error);

// One package of tz.lock: the version a program uses and the hash of its
// sources.
struct Locked {
  std::string id;
  std::string version;     // JSR
  std::string ref, commit; // git
  std::string hash;
  // What it depends on, as its tz.json or its imports write it.
  std::vector<std::string> dependencies;
  std::string describe() const; // "jsr:@std/path 1.0.8"
};
struct Lock {
  std::map<std::string, Locked> packages; // by id
};

// The packages of a program, for the loader. packages[0] is the project.
struct Package {
  std::string id;      // "" for the project
  std::string name;    // what its files are called after: "@std/path/join.ts"
  std::string version;
  std::string dir;     // canonical
  std::string manifestPath; // tz.json, if it has one
  bool external = false;    // from JSR or git: its files stay in `dir`
  bool jsr = false;
  std::string main;
  std::vector<std::pair<std::string, std::string>> exports;
  llvm::StringMap<unsigned> aliases; // import name -> package
  llvm::StringMap<unsigned> jsrDeps; // "@scope/name" of jsr: imports -> package
};
struct Graph {
  std::vector<Package> packages;
};

struct PrepareOptions {
  // tz update: take the newest versions tz.json allows, of the packages in
  // `only` (ids), or of all if it is empty.
  bool update = false;
  std::vector<std::string> only;
};

// Makes tz.lock fit `manifest` (writing it if it changes), puts the sources of
// every package into the cache and describes them in `graph`. `before` and
// `after` get tz.lock as it was and as it is now. Reports errors.
bool preparePackages(const Manifest &manifest, const PrepareOptions &options, Graph &graph,
                     Lock *before = nullptr, Lock *after = nullptr);

// Error output: "tz: message", or "file:line:col: error: message" with the
// line and a caret.
void report(const llvm::Twine &message);
void reportAt(llvm::StringRef file, llvm::StringRef text, size_t offset,
              const llvm::Twine &message);

} // namespace tz::pkg
