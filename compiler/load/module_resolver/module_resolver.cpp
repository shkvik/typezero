#include "compiler/load/module_resolver/module_resolver.h"
#include "compiler/check/sema/sema.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/library_modules/library_modules.h"
#include "compiler/load/parser/parser.h"
#include "compiler/packages/graph/graph.h"
#include "compiler/packages/package/package.h"
#include "compiler/packages/specifier/specifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace tz {

namespace {

// Why an import cannot name `file` (relative to `dir`), which `spec` names as
// written, or "" when its extension is not one of TypeScript or JavaScript:
// the files of tz end in .tz (docs/modules.md).
std::string foreignFile(llvm::StringRef dir, llvm::StringRef file, llvm::StringRef spec) {
  static const llvm::StringLiteral kForeign[] = {".ts",  ".js",  ".tsx", ".jsx",
                                                 ".mts", ".cts", ".mjs", ".cjs"};
  llvm::StringRef ext = path::extension(file, path::Style::posix);
  if (!llvm::is_contained(kForeign, ext)) {
    return "";
  }
  if (file.ends_with(".d.ts")) {
    return "only .tz files can be imported; tz needs no declarations: it reads the code itself";
  }
  std::string stem = spec.drop_back(ext.size()).str();
  std::string message = "the files of tz end in .tz: import '" + stem + "' or '" + stem + ".tz'";
  llvm::SmallString<256> full(dir);
  path::append(full, file);
  if (fs::is_regular_file(full)) {
    message += ", and rename " + path::filename(file, path::Style::posix).str() + " to " +
               path::stem(file, path::Style::posix).str() + ".tz";
  }

  return message;
}

} // namespace

ModuleResolver::ModuleResolver(Loader &loader) : loader_(loader) {}

Module *ModuleResolver::resolve(Module *from, llvm::StringRef spec, llvm::SMLoc loc) {
  // Built into tz (docs/io.md) or into the language (docs/errors.md): Sema
  // binds the names they give.
  if (spec == "neverthrow") {
    return nullptr;
  }
  // Built into tz and written in tz (library/, docs/events.md, docs/net.md);
  // the modules of the globals only for the code there.
  if (spec == "node:events" || spec == "events") {
    return loader_.library().libraryModule("events.tz", "node:events", loc);
  }
  if (spec == "tz:http") {
    return loader_.library().libraryModule("http.tz", "tz:http", loc);
  }
  if (from->builtin && (spec == "tz:web" || spec == "tz:url" || spec == "tz:fetch")) {
    return loader_.library().libraryModule(spec.drop_front(3).str() + ".tz", spec, loc);
  }
  // node:stream and the modules on it (docs/streams.md).
  if (spec == "node:stream" || spec == "stream") {
    return loader_.library().libraryModule("stream.tz", "node:stream", loc);
  }
  if (spec == "node:stream/promises" || spec == "stream/promises") {
    return loader_.library().libraryModule("stream_promises.tz", "node:stream/promises", loc);
  }
  if (spec == "node:stream/consumers" || spec == "stream/consumers") {
    return loader_.library().libraryModule("stream_consumers.tz", "node:stream/consumers", loc);
  }
  if (spec == "node:string_decoder" || spec == "string_decoder") {
    return loader_.library().libraryModule("string_decoder.tz", "node:string_decoder", loc);
  }
  if (isBuiltinModule(spec)) {
    if (std::string problem = builtinModuleProblem(spec); !problem.empty()) {
      loader_.diags_.error(loc, problem);
    }
    return nullptr;
  }
  if (spec.contains('\\')) {
    loader_.diags_.error(loc, "write '/' in the path of an import, not '\\', as on every system");
    return nullptr;
  }
  bool relative = spec == "." || spec == ".." || spec.starts_with("./") || spec.starts_with("../");
  if (!relative) {
    if (spec.starts_with("jsr:") || spec.starts_with("npm:")) {
      return resolvePackage(from, spec, loc);
    }
    if (spec.empty() || path::is_absolute(spec) || spec.starts_with("/") || spec.contains(':')) {
      loader_.diags_.error(
          loc, "an import names a file by its path relative to the importing file, e.g. "
               "'./math' or '../lib/math'");
    } else {
      return resolvePackage(from, spec, loc);
    }
    return nullptr;
  }

  // The files the path may name, in the order they are looked for.
  std::vector<std::string> candidates;
  llvm::StringRef last = path::filename(spec, path::Style::posix);
  llvm::StringRef ext = path::extension(spec, path::Style::posix);
  std::string baseDir = path::parent_path(loader_.realPaths_[from]).str();
  if (spec.ends_with("/") || last == "." || last == "..") {
    candidates.push_back((spec.rtrim('/') + "/index.tz").str());
  } else if (ext == ".tz") {
    candidates.push_back(spec.str());
  } else if (ext == ".json") {
    loader_.diags_.error(loc,
                         "a .json file is not a module: only .tz files can be imported; embed the "
                         "data with 'const data = embedJson(\"" +
                             spec.str() + "\")'");
    return nullptr;
  } else if (std::string problem = foreignFile(baseDir, spec, spec); !problem.empty()) {
    loader_.diags_.error(loc, problem);
    return nullptr;
  } else {
    candidates.push_back((spec + ".tz").str());
    candidates.push_back((spec + "/index.tz").str());
  }

  return load(baseDir, candidates, spec, loc, loader_.packageOf_[from]);
}

Module *ModuleResolver::resolvePackage(Module *from, llvm::StringRef spec, llvm::SMLoc loc) {
  if (spec.starts_with("npm:")) {
    loader_.diags_.error(
        loc, "npm packages are not supported: npm has compiled JavaScript, not "
             "TypeScript sources (docs/packages.md); look for the package on JSR or take "
             "its sources from git");
    return nullptr;
  }
  if (!loader_.packages_) {
    loader_.diags_.error(loc,
                         "'" + spec +
                             "' is the name of a package, but there is no tz.json to list packages "
                             "in; make one with 'tz init' and add the package with 'tz add " +
                             spec +
                             "' (a file of the program is imported by its relative path, e.g. "
                             "'./" +
                             spec + "')");
    return nullptr;
  }
  int fromPackage = loader_.packageOf_[from];
  const pkg::Package &home = loader_.packages_->packages[fromPackage];
  std::string name, subpath;
  const unsigned *target = nullptr;
  if (spec.starts_with("jsr:")) {
    // "jsr:@std/path@^1/join", as JSR writes the imports of the packages it
    // publishes; tz.lock has made sure that the version fits.
    std::string source, inside;
    if (!pkg::splitJsrImport(spec, name, source, inside)) {
      loader_.diags_.error(loc, "'" + spec + "' does not name a JSR package: 'jsr:@scope/name@^1'");
      return nullptr;
    }
    subpath = "." + inside;
    if (!home.jsr) {
      loader_.diags_.error(
          loc, "import a package by its name in tz.json: add it with 'tz add jsr:" + name +
                   "' and write 'from \"" + name +
                   (subpath == "." ? std::string() : subpath.substr(1)) + "\"'");
      return nullptr;
    }
    auto it = home.jsrDeps.find(name);
    if (it != home.jsrDeps.end()) {
      target = &it->second;
    }
  } else {
    if (!pkg::splitPackageSpecifier(spec, name, subpath)) {
      loader_.diags_.error(loc, "'" + spec +
                                    "' does not name a package: a scoped name is '@scope/name'");
      return nullptr;
    }
    auto it = home.aliases.find(name);
    if (it != home.aliases.end()) {
      target = &it->second;
    }
  }
  if (!target) {
    if (fromPackage == 0) {
      loader_.diags_.error(loc, "'" + name + "' is not a dependency in " + home.manifestPath +
                                    "; add it with 'tz add " + name + "'");
    } else if (home.jsr) {
      loader_.diags_.error(loc, "'" + name + "' is not a dependency of " + home.name +
                                    ": a JSR package imports another one as 'jsr:@scope/name@^1'");
    } else {
      loader_.diags_.error(loc,
                           "'" + name + "' is not a dependency of package " + home.name +
                               (home.manifestPath.empty() ? std::string(", which has no tz.json")
                                                          : " (" + home.manifestPath + ")"));
    }
    return nullptr;
  }
  const pkg::Package &package = loader_.packages_->packages[*target];
  int index = static_cast<int>(*target);

  // What the package exports under that path, or, if it does not say, the
  // file of the path.
  std::vector<std::string> candidates;
  if (!package.exports.empty()) {
    std::string listed;
    for (const auto &[key, file] : package.exports) {
      if (key == subpath) {
        candidates.push_back(file);
      }
      listed += (listed.empty() ? "'" : ", '") + name +
                (key == "." ? std::string() : key.substr(1)) + "'";
    }
    if (candidates.empty()) {
      loader_.diags_.error(loc, "'" + spec + "' names nothing " + name + " exports; it exports " +
                                    listed);
      return nullptr;
    }
  } else if (subpath == ".") {
    if (!package.main.empty()) {
      candidates.push_back(package.main);
    } else {
      candidates = {"mod.tz", "index.tz", "src/index.tz", "src/mod.tz"};
    }
  } else {
    llvm::StringRef file = llvm::StringRef(subpath).drop_front(2);
    if (file.ends_with(".tz")) {
      candidates.push_back(file.str());
    } else if (std::string problem = foreignFile(package.dir, file, spec); !problem.empty()) {
      loader_.diags_.error(loc, problem);
      return nullptr;
    } else {
      candidates.push_back((file + ".tz").str());
      candidates.push_back((file + "/index.tz").str());
    }
  }

  return load(package.dir, candidates, spec, loc, index);
}

Module *ModuleResolver::load(llvm::StringRef baseDir, const std::vector<std::string> &candidates,
                             llvm::StringRef spec, llvm::SMLoc loc, int package) {
  const pkg::Package *owner =
      loader_.packages_ && package > 0 ? &loader_.packages_->packages[package] : nullptr;
  bool byName = !spec.starts_with(".");
  for (const std::string &candidate : candidates) {
    llvm::SmallString<256> full(baseDir);
    path::append(full, candidate);
    path::remove_dots(full, /*remove_dot_dot=*/true);
    if (!fs::is_regular_file(full)) {
      continue;
    }
    llvm::SmallString<256> real;
    if (fs::real_path(full, real)) {
      real = full;
    }
    // On a system that ignores the case of names the file is found anyway;
    // elsewhere it would not be.
    if (real.str() != full.str() && real.str().equals_insensitive(full)) {
      loader_.diags_.error(
          loc, "the file is named '" + loader_.relativeName(real) + "', but the import says '" +
                   spec +
                   "'; write the name with the same capital letters, or the program will "
                   "not build where names are case-sensitive");
      return nullptr;
    }
    if (Module *known = loader_.byRealPath_.lookup(real)) {
      return known;
    }
    if (path::extension(real) != ".tz") {
      loader_.diags_.error(loc, "'" + spec + "' names " + path::filename(real) +
                                    ", and the files of tz end in .tz: rename it to " +
                                    path::stem(real) + ".tz");
      return nullptr;
    }
    std::string name = loader_.relativeName(real);
    std::string filePath = name;
    if (owner) {
      // A file of a package is named after the package: "@std/path/join.tz".
      llvm::StringRef inside = real.str();
      bool within =
          inside.consume_front(owner->dir) && (inside.starts_with("/") || inside.starts_with("\\"));
      if (!within && owner->external) {
        loader_.diags_.error(loc,
                             "'" + spec + "' is outside package " + owner->name +
                                 ": a package imports only its own files and its dependencies");
        return nullptr;
      }
      if (within) {
        name = owner->name + "/" + path::convert_to_slash(inside.drop_front());
      }
      filePath = std::string(real);
    } else if (!path::is_absolute(name)) {
      llvm::SmallString<256> joined(loader_.entryDir_);
      path::append(joined, name);
      path::native(joined);
      filePath = std::string(joined);
    }
    std::string error;
    Module *module = loader_.parse(filePath, std::string(real), name, error);
    if (!module) {
      loader_.diags_.error(loc, "cannot read '" + name + "': " + error);
    } else {
      loader_.packageOf_[module] = package;
    }
    return module;
  }

  if (owner && byName) {
    // What the package names (main, exports), a path inside it, or the files
    // a package without a tz.json may start with.
    if (candidates.size() == 1) {
      loader_.diags_.error(loc, "cannot find '" + candidates[0] + "' in package " + owner->name +
                                    " (" + owner->dir + "), which '" + spec + "' names");
    } else if (candidates.size() == 2) {
      loader_.diags_.error(loc, "cannot find '" + spec + "': package " + owner->name + " has no " +
                                    candidates[0] + " or " + candidates[1] + " (in " + owner->dir +
                                    ")");
    } else {
      loader_.diags_.error(loc, "package " + owner->name +
                                    " names no file to import: it has no tz.json with \"main\" or "
                                    "\"exports\", and none of " +
                                    llvm::join(candidates, ", ") + " (in " + owner->dir + ")");
    }
    return nullptr;
  }

  std::string message = "cannot find '" + spec.str() + "': there is no ";
  std::string typescript;
  for (size_t i = 0; i < candidates.size(); ++i) {
    llvm::StringRef shown(candidates[i]);
    if (shown.starts_with("./")) {
      shown = shown.drop_front(2);
    }
    message += (i ? " or " : "") + shown.str();
    // The same file of TypeScript, which only needs another name.
    llvm::SmallString<256> ts(baseDir);
    path::append(ts, shown.drop_back(3) + ".ts");
    if (typescript.empty() && fs::is_regular_file(ts)) {
      typescript = path::convert_to_slash(shown.drop_back(3)) + ".ts";
    }
  }
  if (!typescript.empty()) {
    message += "; there is " + typescript + ", and the files of tz end in .tz: rename it to " +
               typescript.substr(0, typescript.size() - 3) + ".tz";
  }
  loader_.diags_.error(loc, message);

  return nullptr;
}

} // namespace tz
