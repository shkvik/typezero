#include "pkg/commands.h"

#include "pkg/fetch.h"
#include "pkg/json.h"
#include "pkg/project.h"
#include "pkg/semver.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

int usage(const llvm::Twine &message, llvm::StringRef synopsis) {
  llvm::errs() << "tz: " << message << "\n\nusage: " << synopsis << "\n";
  return kExitUsage;
}

bool writeText(llvm::StringRef file, llvm::StringRef text) {
  std::string error;
  if (writeFileAtomically(file, text, error))
    return true;
  report(error);
  return false;
}

// The project of the current directory.
bool currentProject(Manifest &out) {
  std::string file = findManifest("");
  if (file.empty()) {
    report("there is no tz.json in this directory or above it; make one with 'tz init'");
    return false;
  }
  return readManifest(file, out);
}

// `to` relative to the directory `from`, with '/': "../lib".
std::string relativePath(llvm::StringRef from, llvm::StringRef to) {
  std::vector<llvm::StringRef> base(path::begin(from), path::end(from));
  std::vector<llvm::StringRef> target(path::begin(to), path::end(to));
  size_t common = 0;
  while (common < base.size() && common < target.size() &&
         base[common].equals_insensitive(target[common]))
    ++common;
  if (common == 0)
    return path::convert_to_slash(to);
  std::string result;
  for (size_t i = common; i < base.size(); ++i)
    result += "../";
  for (size_t i = common; i < target.size(); ++i)
    result += target[i].str() + "/";
  if (result.empty())
    return ".";
  result.pop_back();
  return result.starts_with("../") ? result : "./" + result;
}

// Prepares the packages of the project as tz.json would be with `deps`, and
// writes tz.json if that works.
bool applyDependencies(const Manifest &project, const std::vector<Dependency> &deps,
                       const PrepareOptions &options, Lock *before, Lock *after) {
  std::string text = withDependencies(project, deps);
  Manifest edited;
  if (!readManifestText(project.path, text, edited))
    return false;
  Graph graph;
  if (!preparePackages(edited, options, graph, before, after))
    return false;
  return text == project.text || writeText(project.path, text);
}

// "/x", "C:/x", "C:\x": a path, not a source with a scheme like "git:".
bool isAbsolutePath(llvm::StringRef text) {
  return text.starts_with("/") || text.starts_with("\\") ||
         (text.size() > 2 && llvm::isAlpha(text[0]) && text[1] == ':' &&
          (text[2] == '/' || text[2] == '\\'));
}

// What `tz add <arg>` adds: the name imports will use and the source for
// tz.json.
bool interpretAdd(const Manifest &project, llvm::StringRef arg, Dependency &out) {
  std::string error;
  Source source;
  llvm::StringRef text = arg;
  bool bare = !text.starts_with("jsr:") && !text.starts_with("@") && !text.contains(':') &&
              !text.starts_with(".") && !isAbsolutePath(text);
  if (bare || text.starts_with("@") || text.starts_with("jsr:")) {
    // A name alone is a package adapted for tz: "zod" is jsr:@tz/zod
    // (docs/packages.md, section 2), imported as "zod".
    std::string spec;
    if (bare) {
      auto [name, range] = text.split('@');
      spec = "jsr:@tz/" + name.str() + (range.empty() ? "" : "@" + range.str());
    } else {
      spec = text.starts_with("jsr:") ? text.str() : "jsr:" + text.str();
    }
    if (!parseSource(spec, project.dir, source, error)) {
      report(error);
      return false;
    }
    out.alias = source.scope == "tz" ? source.name : "@" + source.scope + "/" + source.name;
    if (source.range.empty()) {
      // The newest version, written as the range of what is compatible with
      // it.
      JsrVersions versions;
      if (!jsrVersions(source.scope, source.name, versions, error)) {
        if (!error.empty())
          report(error);
        else if (bare)
          report("'" + source.name + "' has not been adapted for tz yet: there is no " +
                 source.id() + " in the registry; give its source, e.g. 'tz add "
                 "github:user/repo@v1.0.0', or adapt it with 'tz port'");
        else
          report("there is no package " + source.id() + " in the registry (" + jsrBaseUrl() + ")");
        return false;
      }
      const Version *newest = nullptr, *newestPre = nullptr;
      for (const JsrVersions::Entry &entry : versions.versions) {
        if (entry.yanked)
          continue;
        const Version *&slot = entry.version.isPrerelease() ? newestPre : newest;
        if (!slot || *slot < entry.version)
          slot = &entry.version;
      }
      if (!newest)
        newest = newestPre;
      if (!newest) {
        report(source.id() + " has no versions");
        return false;
      }
      spec += "@^" + newest->str();
    }
    out.spec = spec;
    return checkAlias(out.alias, error) || (report(error), false);
  }

  std::string spec = isAbsolutePath(text) ? "file:" + text.str() : text.str();
  if (!parseSource(spec, project.dir, source, error)) {
    report(error);
    return false;
  }
  Manifest package;
  if (source.kind == Source::Kind::Local) {
    if (!readPackageDir(source.dir, package))
      return false;
    out.spec = "file:" + relativePath(project.dir, source.dir);
  } else {
    // The package's own name is the import name, if it has one.
    std::string commit, dir;
    if (!gitResolve(source.url, source.ref, /*refresh=*/true, commit, error) ||
        !gitCheckout(source.url, commit, dir, error)) {
      report(error);
      return false;
    }
    if (!readPackageDir(dir, package))
      return false;
    out.spec = text.str();
  }
  out.alias = package.name.empty() ? source.shortName() : package.name;
  if (!checkAlias(out.alias, error)) {
    report(error + "; add the package to tz.json by hand, under the name its imports use");
    return false;
  }
  return true;
}

void printChanges(const Lock &before, const Lock &after) {
  bool any = false;
  for (const auto &[id, now] : after.packages) {
    auto old = before.packages.find(id);
    if (old == before.packages.end()) {
      llvm::outs() << "added " << now.describe() << "\n";
      any = true;
    } else if (old->second.version != now.version || old->second.commit != now.commit) {
      llvm::outs() << "updated " << id << " "
                   << (now.version.empty() ? old->second.commit.substr(0, 12) : old->second.version)
                   << " -> "
                   << (now.version.empty() ? now.commit.substr(0, 12) : now.version) << "\n";
      any = true;
    }
  }
  for (const auto &[id, old] : before.packages)
    if (!after.packages.count(id)) {
      llvm::outs() << "removed " << old.describe() << "\n";
      any = true;
    }
  if (!any)
    llvm::outs() << "the packages are up to date\n";
}

} // namespace

int commandInit(llvm::ArrayRef<const char *> args) {
  if (args.size() > 1)
    return usage("'tz init' takes one directory", "tz init [directory]");
  llvm::SmallString<256> dir;
  if (args.empty()) {
    fs::current_path(dir);
  } else {
    dir = args[0];
    fs::make_absolute(dir);
    if (std::error_code ec = fs::create_directories(dir)) {
      report("cannot create '" + dir + "': " + ec.message());
      return kExitError;
    }
  }
  path::remove_dots(dir, /*remove_dot_dot=*/true);
  auto file = [&](llvm::StringRef name) {
    llvm::SmallString<256> result(dir);
    path::append(result, name);
    return std::string(result);
  };
  if (fs::exists(file("tz.json"))) {
    report("there is a tz.json already in " + dir);
    return kExitError;
  }
  std::string name = path::filename(dir).str();
  std::string error;
  if (!checkAlias(name, error))
    name = "app";
  std::vector<std::string> created{"tz.json"};
  if (!writeText(file("tz.json"), "{\n  \"name\": " + jsonQuote(name) +
                                      ",\n  \"main\": \"main.ts\",\n  \"dependencies\": {}\n}\n"))
    return kExitError;
  if (!fs::exists(file("main.ts"))) {
    if (!writeText(file("main.ts"), "console.log(\"Hello, world!\")\n"))
      return kExitError;
    created.push_back("main.ts");
  }
  if (!fs::exists(file(".gitignore"))) {
    // What tz build makes. tz.lock belongs in git: it makes every build use
    // the same sources.
    if (!writeText(file(".gitignore"), "*.exe\n*.ll\n/" + name + "\n"))
      return kExitError;
    created.push_back(".gitignore");
  }
  llvm::outs() << "created ";
  for (size_t i = 0; i < created.size(); ++i)
    llvm::outs() << (i == 0 ? "" : i + 1 == created.size() ? " and " : ", ") << created[i];
  llvm::outs() << (args.empty() ? std::string() : " in " + std::string(args[0]))
               << "; run the program with 'tz run'\n";
  return 0;
}

int commandAdd(llvm::ArrayRef<const char *> args) {
  const char *synopsis = "tz add <package>...\n\n"
                         "  tz add zod                     zod adapted for tz: jsr:@tz/zod\n"
                         "  tz add @std/path@^1            a JSR package\n"
                         "  tz add github:user/repo@v1.0   a GitHub repository at a tag\n"
                         "  tz add git:<address>@v1.0      any git repository\n"
                         "  tz add ../lib                  a directory";
  if (args.empty())
    return usage("'tz add' needs a package", synopsis);
  for (llvm::StringRef arg : args)
    if (arg.starts_with("-"))
      return usage("unknown option '" + arg + "'", synopsis);
  Manifest project;
  if (!currentProject(project))
    return kExitError;
  std::vector<Dependency> deps = project.dependencies;
  PrepareOptions options;
  options.update = true;
  std::vector<Dependency> added;
  for (llvm::StringRef arg : args) {
    Dependency dep;
    if (!interpretAdd(project, arg, dep))
      return kExitError;
    auto it = std::find_if(deps.begin(), deps.end(),
                           [&](const Dependency &d) { return d.alias == dep.alias; });
    if (it != deps.end())
      it->spec = dep.spec;
    else
      deps.push_back(dep);
    Source source;
    std::string error;
    if (parseSource(dep.spec, project.dir, source, error))
      options.only.push_back(source.id());
    added.push_back(dep);
  }
  Lock before, after;
  if (!applyDependencies(project, deps, options, &before, &after))
    return kExitError;
  for (const Dependency &dep : added) {
    llvm::outs() << "added " << dep.alias << ": " << dep.spec;
    Source source;
    std::string error;
    if (parseSource(dep.spec, project.dir, source, error))
      if (auto it = after.packages.find(source.id()); it != after.packages.end())
        llvm::outs() << " ("
                     << (it->second.version.empty() ? it->second.commit.substr(0, 12)
                                                    : it->second.version)
                     << ")";
    llvm::outs() << "\n";
  }
  return 0;
}

int commandRemove(llvm::ArrayRef<const char *> args) {
  if (args.empty())
    return usage("'tz remove' needs the name of a package", "tz remove <name>...");
  Manifest project;
  if (!currentProject(project))
    return kExitError;
  std::vector<Dependency> deps = project.dependencies;
  for (llvm::StringRef name : args) {
    auto it = std::find_if(deps.begin(), deps.end(),
                           [&](const Dependency &d) { return d.alias == name; });
    if (it == deps.end()) {
      report("'" + name + "' is not a dependency in " + project.path);
      return kExitError;
    }
    deps.erase(it);
  }
  if (!applyDependencies(project, deps, PrepareOptions(), nullptr, nullptr))
    return kExitError;
  for (llvm::StringRef name : args)
    llvm::outs() << "removed " << name << "\n";
  return 0;
}

int commandUpdate(llvm::ArrayRef<const char *> args) {
  Manifest project;
  if (!currentProject(project))
    return kExitError;
  PrepareOptions options;
  options.update = true;
  for (llvm::StringRef name : args) {
    auto it = std::find_if(project.dependencies.begin(), project.dependencies.end(),
                           [&](const Dependency &d) { return d.alias == name; });
    if (it == project.dependencies.end()) {
      report("'" + name + "' is not a dependency in " + project.path);
      return kExitError;
    }
    Source source;
    std::string error;
    if (!parseSource(it->spec, project.dir, source, error)) {
      reportAt(project.path, project.text, it->offset, error);
      return kExitError;
    }
    options.only.push_back(source.id());
  }
  Graph graph;
  Lock before, after;
  if (!preparePackages(project, options, graph, &before, &after))
    return kExitError;
  printChanges(before, after);
  return 0;
}

} // namespace tz::pkg
