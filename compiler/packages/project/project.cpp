#include "compiler/packages/project/project.h"

#include "compiler/program/ast/ast.h"
#include "compiler/packages/fetch/fetch.h"
#include "compiler/packages/json/json.h"
#include "compiler/packages/scan/scan.h"
#include "compiler/packages/semver/semver.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <set>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

void report(const llvm::Twine &message) { llvm::errs() << "tz: " << message << "\n"; }

void reportAt(llvm::StringRef file, llvm::StringRef text, size_t offset,
              const llvm::Twine &message) {
  llvm::SourceMgr sourceMgr;
  unsigned id =
      sourceMgr.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(text, file), llvm::SMLoc());
  const char *start = sourceMgr.getMemoryBuffer(id)->getBufferStart();
  llvm::SMLoc loc = llvm::SMLoc::getFromPointer(start + std::min(offset, text.size()));
  sourceMgr.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Error, message);
}

namespace {

std::string canonical(llvm::StringRef dir) {
  llvm::SmallString<256> real;
  if (fs::real_path(dir, real)) {
    real = dir;
    fs::make_absolute(real);
    path::remove_dots(real, /*remove_dot_dot=*/true);
  }
  return std::string(real);
}

bool isJsrNamePart(llvm::StringRef part) {
  return !part.empty() && llvm::all_of(part, [](char c) {
           return llvm::isLower(c) || llvm::isDigit(c) || c == '-';
         });
}

// The repository's name from its address: "https://github.com/a/uuid.git" ->
// "uuid".
std::string repositoryName(llvm::StringRef url) {
  llvm::StringRef name = url.rtrim("/");
  size_t slash = name.find_last_of("/:\\");
  if (slash != llvm::StringRef::npos)
    name = name.drop_front(slash + 1);
  name.consume_back(".git");
  return name.str();
}

} // namespace

std::string Source::id() const {
  switch (kind) {
  case Kind::Jsr: return "jsr:@" + scope + "/" + name;
  case Kind::Git: return "git:" + url;
  case Kind::Local: return "file:" + path::convert_to_slash(dir);
  }
  return "";
}

std::string Source::shortName() const {
  switch (kind) {
  case Kind::Jsr: return "@" + scope + "/" + name;
  case Kind::Git: return repositoryName(url);
  case Kind::Local: return path::filename(dir).str();
  }
  return "";
}

bool parseSource(llvm::StringRef spec, llvm::StringRef baseDir, Source &out, std::string &error) {
  out = Source();
  llvm::StringRef rest = spec;
  if (rest.consume_front("jsr:")) {
    out.kind = Source::Kind::Jsr;
    rest.consume_front("/");
    // "@scope/name" and then "@range".
    llvm::StringRef scope, name;
    if (rest.consume_front("@")) {
      std::tie(scope, rest) = rest.split('/');
      size_t at = rest.find('@');
      name = rest.substr(0, at);
      rest = at == llvm::StringRef::npos ? llvm::StringRef() : rest.drop_front(at + 1);
    }
    if (!isJsrNamePart(scope) || !isJsrNamePart(name)) {
      error = "'" + spec.str() + "' does not name a JSR package: write 'jsr:@scope/name' and a "
              "range of versions, e.g. 'jsr:@std/path@^1'";
      return false;
    }
    out.scope = scope.str();
    out.name = name.str();
    out.range = rest.str();
    Range range;
    if (!Range::parse(out.range, range, error)) {
      error = "in '" + spec.str() + "': " + error;
      return false;
    }
    return true;
  }
  if (rest.consume_front("github:") || rest.consume_front("git:")) {
    out.kind = Source::Kind::Git;
    bool github = spec.starts_with("github:");
    // The ref follows the last '@' after the last '/': "git@host:a/b.git" has
    // none, "git@host:a/b.git@v1" has "v1".
    size_t slash = rest.find_last_of('/');
    size_t at = rest.find_last_of('@');
    llvm::StringRef where = rest;
    if (at != llvm::StringRef::npos && (slash == llvm::StringRef::npos || at > slash)) {
      where = rest.substr(0, at);
      out.ref = rest.drop_front(at + 1).str();
    }
    // Not an option of git.
    if (where.starts_with("-") || llvm::StringRef(out.ref).starts_with("-")) {
      error = "'" + spec.str() + "': an address or a tag of git does not start with '-'";
      return false;
    }
    if (github) {
      auto [user, repo] = where.split('/');
      if (user.empty() || repo.empty() || repo.contains('/')) {
        error = "'" + spec.str() + "' does not name a GitHub repository: write "
                "'github:user/repo@v1.2.0'";
        return false;
      }
      repo.consume_back(".git");
      out.url = "https://github.com/" + user.str() + "/" + repo.str() + ".git";
    } else if (where.starts_with("./") || where.starts_with("../")) {
      llvm::SmallString<256> full(baseDir);
      path::append(full, where);
      path::remove_dots(full, /*remove_dot_dot=*/true);
      out.url = path::convert_to_slash(full);
    } else {
      out.url = where.str();
    }
    if (out.url.empty()) {
      error = "'" + spec.str() + "' names no repository: write 'git:' and what 'git clone' takes, "
              "e.g. 'git:https://example.com/lib.git@v1.0.0'";
      return false;
    }
    return true;
  }
  bool local = rest.consume_front("file:");
  if (local || rest.starts_with("./") || rest.starts_with("../") || rest == "." || rest == "..") {
    out.kind = Source::Kind::Local;
    llvm::SmallString<256> full(rest);
    if (!path::is_absolute(full)) {
      full = baseDir;
      path::append(full, rest);
    }
    if (!fs::is_directory(full)) {
      error = "there is no directory '" + rest.str() + "'" +
              (baseDir.empty() ? std::string() : " in " + baseDir.str());
      return false;
    }
    out.dir = canonical(full);
    return true;
  }
  if (rest.starts_with("npm:")) {
    error = "npm packages are not supported: npm has compiled JavaScript, not TypeScript sources "
            "(docs/packages.md); look for the package on JSR ('jsr:@scope/name@^1') or take its "
            "sources from git ('github:user/repo@v1.0.0')";
    return false;
  }
  Range range;
  std::string ignored;
  if (!rest.empty() && (rest == "latest" || Range::parse(rest, range, ignored))) {
    error = "'" + spec.str() + "' is a version, but not where the package comes from; tz takes "
            "packages from JSR and git: write e.g. 'jsr:@scope/name@" +
            (rest == "latest" ? std::string("^1") : rest.str()) + "'";
    return false;
  }
  error = "'" + spec.str() + "' does not say where the package comes from; write "
          "'jsr:@scope/name@^1', 'github:user/repo@v1.0.0', 'git:<address>@v1.0.0' or "
          "'file:<directory>'";
  return false;
}

bool splitPackageSpecifier(llvm::StringRef specifier, std::string &name, std::string &subpath) {
  llvm::StringRef rest = specifier;
  size_t end = rest.find('/');
  if (rest.starts_with("@")) {
    if (end == llvm::StringRef::npos)
      return false;
    end = rest.find('/', end + 1);
  }
  name = rest.substr(0, end).str();
  subpath = end == llvm::StringRef::npos ? "." : "./" + rest.drop_front(end + 1).str();
  return !name.empty();
}

bool splitJsrImport(llvm::StringRef specifier, std::string &package, std::string &source,
                    std::string &subpath) {
  llvm::StringRef rest = specifier;
  if (!rest.consume_front("jsr:"))
    return false;
  rest.consume_front("/");
  if (!rest.starts_with("@"))
    return false;
  size_t slash = rest.find('/');
  size_t end = slash == llvm::StringRef::npos ? slash : rest.find('/', slash + 1);
  llvm::StringRef full = rest.substr(0, end); // "@std/path@^1"
  package = full.substr(0, full.find('@', 1)).str();
  source = "jsr:" + full.str();
  subpath = end == llvm::StringRef::npos ? "" : rest.drop_front(end).str();
  return true;
}

bool checkAlias(llvm::StringRef alias, std::string &error) {
  auto validPart = [](llvm::StringRef part) {
    return !part.empty() && !part.starts_with(".") &&
           llvm::all_of(part, [](char c) {
             return llvm::isAlnum(c) || c == '-' || c == '_' || c == '.';
           });
  };
  bool valid;
  if (alias.starts_with("@")) {
    auto [scope, name] = alias.drop_front().split('/');
    valid = validPart(scope) && validPart(name);
  } else {
    valid = validPart(alias);
  }
  if (!valid) {
    error = "'" + alias.str() + "' cannot be imported as a package name: write a name like "
            "'zod' or '@std/path'";
    return false;
  }
  if (isBuiltinModule(alias) || alias == "neverthrow") {
    error = "'" + alias.str() + "' is the name of a module built into tz; give the package "
            "another name";
    return false;
  }
  return true;
}

std::string findManifest(llvm::StringRef dir) {
  llvm::SmallString<256> current(dir);
  if (current.empty())
    fs::current_path(current);
  fs::make_absolute(current);
  path::remove_dots(current, /*remove_dot_dot=*/true);
  while (!current.empty()) {
    llvm::SmallString<256> candidate(current);
    path::append(candidate, "tz.json");
    if (fs::is_regular_file(candidate))
      return std::string(candidate);
    llvm::StringRef parent = path::parent_path(current);
    if (parent == current)
      break;
    current = parent;
  }
  return "";
}

namespace {

bool readText(llvm::StringRef file, std::string &text) {
  auto buffer = llvm::MemoryBuffer::getFile(file, /*IsText=*/true);
  if (!buffer) {
    report("cannot read '" + file + "': " + buffer.getError().message());
    return false;
  }
  text = (*buffer)->getBuffer().str();
  return true;
}

bool readExports(const Manifest &out, const JsonValue &value,
                 std::vector<std::pair<std::string, std::string>> &exports) {
  auto add = [&](llvm::StringRef key, const JsonValue &target) {
    if (!target.isString()) {
      reportAt(out.path, out.text, target.begin, "an export names a .tz file of the package");
      return false;
    }
    llvm::StringRef file = target.string;
    file.consume_front("./");
    exports.push_back({key.str(), file.str()});
    return true;
  };
  if (value.isString())
    return add(".", value);
  if (!value.isObject()) {
    reportAt(out.path, out.text, value.begin,
             "'exports' is a file, or an object such as { \".\": \"./mod.tz\", \"./mini\": "
             "\"./mini.tz\" }");
    return false;
  }
  for (size_t i = 0; i < value.keys.size(); ++i) {
    llvm::StringRef key = value.keys[i];
    if (key != "." && !key.starts_with("./")) {
      reportAt(out.path, out.text, value.keyOffsets[i],
               "an export is named '.' or './name', as imports write it after the package's "
               "name");
      return false;
    }
    if (!add(key, value.items[i]))
      return false;
  }
  return true;
}

bool parseManifest(Manifest &out) {
  JsonValue root;
  size_t at;
  std::string error;
  if (!parseJson(out.text, root, at, error)) {
    reportAt(out.path, out.text, at, error);
    return false;
  }
  if (!root.isObject()) {
    reportAt(out.path, out.text, root.begin, "tz.json holds an object: { \"name\": ..., "
                                             "\"dependencies\": { ... } }");
    return false;
  }
  out.rootBegin = root.begin;
  out.rootEnd = root.end;
  if (!root.keys.empty()) {
    out.lastMemberEnd = root.items.back().end;
    // The indentation of the first member.
    size_t lineStart = out.text.rfind('\n', root.keyOffsets[0]);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    llvm::StringRef lead = llvm::StringRef(out.text).slice(lineStart, root.keyOffsets[0]);
    if (!lead.empty() && llvm::all_of(lead, [](char c) { return c == ' ' || c == '\t'; }))
      out.indent = lead.str();
  }
  for (size_t i = 0; i < root.keys.size(); ++i) {
    llvm::StringRef key = root.keys[i];
    const JsonValue &value = root.items[i];
    if (key == "name" || key == "version" || key == "main") {
      if (!value.isString()) {
        reportAt(out.path, out.text, value.begin, "'" + key + "' is a string");
        return false;
      }
      (key == "name" ? out.name : key == "version" ? out.version : out.main) = value.string;
      if (key == "main")
        out.mainOffset = value.begin;
    } else if (key == "exports") {
      if (!readExports(out, value, out.exports))
        return false;
    } else if (key == "dependencies") {
      if (!value.isObject()) {
        reportAt(out.path, out.text, value.begin,
                 "'dependencies' is an object: { \"zod\": \"jsr:@tz/zod@^4\" }");
        return false;
      }
      out.depsBegin = value.begin;
      out.depsEnd = value.end;
      for (size_t j = 0; j < value.keys.size(); ++j) {
        if (!checkAlias(value.keys[j], error)) {
          reportAt(out.path, out.text, value.keyOffsets[j], error);
          return false;
        }
        const JsonValue &spec = value.items[j];
        if (!spec.isString()) {
          reportAt(out.path, out.text, spec.begin,
                   "a dependency is a string that says where the package comes from, e.g. "
                   "\"jsr:@std/path@^1\"");
          return false;
        }
        out.dependencies.push_back({value.keys[j], spec.string, spec.begin});
      }
    } else {
      reportAt(out.path, out.text, root.keyOffsets[i],
               "unknown field '" + key +
                   "' in tz.json; the fields are name, version, main, exports and dependencies");
      return false;
    }
  }
  return true;
}

} // namespace

bool readManifest(llvm::StringRef file, Manifest &out) {
  std::string text;
  return readText(file, text) && readManifestText(file, std::move(text), out);
}

bool readManifestText(llvm::StringRef file, std::string text, Manifest &out) {
  out = Manifest();
  out.path = file.str();
  out.dir = canonical(path::parent_path(file));
  out.isTzJson = true;
  out.text = std::move(text);
  return parseManifest(out);
}

bool readPackageDir(llvm::StringRef dir, Manifest &out) {
  llvm::SmallString<256> file(dir);
  path::append(file, "tz.json");
  if (fs::is_regular_file(file))
    return readManifest(file, out);
  out = Manifest();
  out.dir = canonical(dir);
  // A package made for Deno or JSR: its name and exports, no more.
  for (const char *name : {"jsr.json", "deno.json", "deno.jsonc"}) {
    file = dir;
    path::append(file, name);
    if (!fs::is_regular_file(file))
      continue;
    out.path = std::string(file);
    if (!readText(file, out.text))
      return false;
    JsonValue root;
    size_t at;
    std::string error;
    if (!parseJson(out.text, root, at, error)) {
      reportAt(out.path, out.text, at, error);
      return false;
    }
    out.name = root.getString("name");
    out.version = root.getString("version");
    if (const JsonValue *exports = root.get("exports"))
      if (!readExports(out, *exports, out.exports))
        return false;
    break;
  }
  return true;
}

std::string withDependencies(const Manifest &m, std::vector<Dependency> deps) {
  std::sort(deps.begin(), deps.end(),
            [](const Dependency &a, const Dependency &b) { return a.alias < b.alias; });
  const std::string &indent = m.indent;
  std::string object = "{}";
  if (!deps.empty()) {
    object = "{\n";
    for (size_t i = 0; i < deps.size(); ++i)
      object += indent + indent + jsonQuote(deps[i].alias) + ": " + jsonQuote(deps[i].spec) +
                (i + 1 < deps.size() ? ",\n" : "\n");
    object += indent + "}";
  }
  const std::string &text = m.text;
  if (m.depsBegin != std::string::npos)
    return text.substr(0, m.depsBegin) + object + text.substr(m.depsEnd);
  if (m.lastMemberEnd != std::string::npos)
    return text.substr(0, m.lastMemberEnd) + ",\n" + indent + "\"dependencies\": " + object +
           text.substr(m.lastMemberEnd);
  return text.substr(0, m.rootBegin) + "{\n" + indent + "\"dependencies\": " + object + "\n}" +
         text.substr(m.rootEnd);
}

std::string Locked::describe() const {
  if (!version.empty())
    return id + " " + version;
  if (!commit.empty())
    return id + (ref.empty() ? std::string() : " " + ref) + " (" + commit.substr(0, 12) + ")";
  return id;
}

namespace {

// --- tz.lock -----------------------------------------------------------------

constexpr int kLockVersion = 1;

bool readLock(llvm::StringRef file, Lock &out) {
  out = Lock();
  if (!fs::exists(file))
    return true;
  std::string text;
  if (!readText(file, text))
    return false;
  JsonValue root;
  size_t at;
  std::string error;
  if (!parseJson(text, root, at, error)) {
    reportAt(file, text, at, error + "; delete tz.lock to make it anew");
    return false;
  }
  const JsonValue *version = root.get("lockVersion");
  if (!version || version->kind != JsonValue::Kind::Number || version->number != kLockVersion) {
    reportAt(file, text, version ? version->begin : root.begin,
             "tz.lock is not in the format of this tz (\"lockVersion\": 1)");
    return false;
  }
  const JsonValue *packages = root.get("packages");
  if (!packages)
    return true;
  for (size_t i = 0; packages->isObject() && i < packages->keys.size(); ++i) {
    const JsonValue &entry = packages->items[i];
    Locked locked;
    locked.id = packages->keys[i];
    locked.version = entry.getString("version");
    locked.ref = entry.getString("ref");
    locked.commit = entry.getString("commit");
    locked.hash = entry.getString("hash");
    if (const JsonValue *deps = entry.get("dependencies"))
      for (const JsonValue &dep : deps->items)
        if (dep.isString())
          locked.dependencies.push_back(dep.string);
    bool jsr = llvm::StringRef(locked.id).starts_with("jsr:");
    bool git = llvm::StringRef(locked.id).starts_with("git:");
    if ((!jsr && !git) || (jsr && locked.version.empty()) || (git && locked.commit.empty()) ||
        locked.hash.empty()) {
      reportAt(file, text, packages->keyOffsets[i],
               "tz.lock describes '" + locked.id + "' wrongly; delete tz.lock to make it anew");
      return false;
    }
    out.packages[locked.id] = std::move(locked);
  }
  return true;
}

std::string lockText(const Lock &lock) {
  std::string text = "{\n  \"lockVersion\": " + std::to_string(kLockVersion) + ",\n";
  text += "  \"packages\": {";
  size_t n = 0;
  for (const auto &[id, p] : lock.packages) {
    text += n++ ? ",\n" : "\n";
    text += "    " + jsonQuote(id) + ": {\n";
    std::vector<std::string> fields;
    if (!p.version.empty())
      fields.push_back("\"version\": " + jsonQuote(p.version));
    if (!p.ref.empty())
      fields.push_back("\"ref\": " + jsonQuote(p.ref));
    if (!p.commit.empty())
      fields.push_back("\"commit\": " + jsonQuote(p.commit));
    fields.push_back("\"hash\": " + jsonQuote(p.hash));
    if (!p.dependencies.empty()) {
      std::string deps = "\"dependencies\": [";
      for (size_t i = 0; i < p.dependencies.size(); ++i)
        deps += (i ? ", " : "") + jsonQuote(p.dependencies[i]);
      fields.push_back(deps + "]");
    }
    for (size_t i = 0; i < fields.size(); ++i)
      text += "      " + fields[i] + (i + 1 < fields.size() ? ",\n" : "\n");
    text += "    }";
  }
  text += n ? "\n  }\n}\n" : "}\n}\n";
  return text;
}

bool writeText(llvm::StringRef file, llvm::StringRef text) {
  std::string error;
  if (writeFileAtomically(file, text, error))
    return true;
  report(error);
  return false;
}

// --- resolution ----------------------------------------------------------------

// Something that needs a package: tz.json or another package.
struct Requirement {
  std::string by; // "tz.json", "jsr:@a/b 1.0.0"
  std::string spec;
  Source source;
};

class Resolver {
public:
  Resolver(const Manifest &root, const Lock &old, const PrepareOptions &options)
      : root_(root), old_(old), options_(options) {}

  bool run(Lock &out);
  // The local packages reached, by id.
  std::map<std::string, Manifest> locals;

private:
  bool collect(std::map<std::string, std::vector<Requirement>> &reqs);
  bool updating(const std::string &id) const {
    return options_.update &&
           (options_.only.empty() ||
            std::find(options_.only.begin(), options_.only.end(), id) != options_.only.end());
  }
  bool fits(const Locked &locked, const std::vector<Requirement> &reqs) const;
  bool choose(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  bool chooseJsr(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  bool chooseGit(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  // Reports a dependency that is written wrongly, in the file that writes it.
  void reportSpec(const std::string &by, const Dependency *dep, const std::string &message);

  const Manifest &root_;
  const Lock &old_;
  const PrepareOptions &options_;
  std::map<std::string, Locked> chosen_;
  std::map<std::string, JsrVersions> versions_;
};

void Resolver::reportSpec(const std::string &by, const Dependency *dep,
                          const std::string &message) {
  const Manifest *manifest = nullptr;
  if (by == "tz.json")
    manifest = &root_;
  for (const auto &[id, local] : locals)
    if (by == local.path)
      manifest = &local;
  if (manifest && dep)
    reportAt(manifest->path, manifest->text, dep->offset, message);
  else
    report(by + ": " + message);
}

bool Resolver::collect(std::map<std::string, std::vector<Requirement>> &reqs) {
  reqs.clear();
  struct Node {
    std::string by;
    std::string baseDir;
    bool allowLocal;
    const std::vector<Dependency> *deps;  // from a tz.json
    const std::vector<std::string> *specs; // from tz.lock
  };
  std::deque<Node> queue{{"tz.json", root_.dir, true, &root_.dependencies, nullptr}};
  std::set<std::string> visited;
  while (!queue.empty()) {
    Node node = queue.front();
    queue.pop_front();
    size_t count = node.deps ? node.deps->size() : node.specs->size();
    for (size_t i = 0; i < count; ++i) {
      const Dependency *dep = node.deps ? &(*node.deps)[i] : nullptr;
      const std::string &spec = dep ? dep->spec : (*node.specs)[i];
      Source source;
      std::string error;
      if (!parseSource(spec, node.baseDir, source, error)) {
        reportSpec(node.by, dep, error);
        return false;
      }
      if (source.kind == Source::Kind::Local && !node.allowLocal) {
        reportSpec(node.by, dep,
                   "a package from JSR or git cannot depend on a directory of this machine ('" +
                       spec + "')");
        return false;
      }
      std::string id = source.id();
      reqs[id].push_back({node.by, spec, source});
      if (!visited.insert(id).second)
        continue;
      if (source.kind == Source::Kind::Local) {
        auto it = locals.find(id);
        if (it == locals.end()) {
          Manifest local;
          if (!readPackageDir(source.dir, local))
            return false;
          it = locals.emplace(id, std::move(local)).first;
        }
        const Manifest &local = it->second;
        std::string by = local.path.empty() ? source.dir : local.path;
        queue.push_back({by, local.dir, true, &local.dependencies, nullptr});
      } else if (auto it = chosen_.find(id); it != chosen_.end()) {
        queue.push_back({it->second.describe(), "", false, nullptr, &it->second.dependencies});
      }
    }
  }
  return true;
}

bool Resolver::fits(const Locked &locked, const std::vector<Requirement> &reqs) const {
  for (const Requirement &req : reqs) {
    if (req.source.kind == Source::Kind::Jsr) {
      Range range;
      Version version;
      std::string ignored;
      if (!Range::parse(req.source.range, range, ignored) ||
          !Version::parse(locked.version, version) || !range.satisfies(version))
        return false;
    } else if (req.source.kind == Source::Kind::Git) {
      if (req.source.ref != locked.ref)
        return false;
    }
  }
  return true;
}

bool Resolver::choose(const std::string &id, const std::vector<Requirement> &reqs, Locked &out) {
  out = Locked();
  out.id = id;
  if (reqs.front().source.kind == Source::Kind::Jsr)
    return chooseJsr(id, reqs, out);
  return chooseGit(id, reqs, out);
}

// The dependencies of a JSR package: the jsr: imports of the files its
// exports reach. JSR writes them so when it publishes ("jsr:@std/path@^1/join").
std::vector<std::string> jsrDependencies(const std::string &dir, const JsrVersion &meta) {
  std::vector<std::string> entries, files, external, deps;
  for (const auto &[name, file] : meta.exports) {
    llvm::SmallString<256> full(dir);
    path::append(full, file);
    entries.push_back(std::string(full));
  }
  reachableFiles(entries, files, external, Sources::Tz);
  for (llvm::StringRef spec : external) {
    // Without the path inside the package: "jsr:@std/path@^1".
    std::string package, dep, subpath;
    if (splitJsrImport(spec, package, dep, subpath) &&
        std::find(deps.begin(), deps.end(), dep) == deps.end())
      deps.push_back(dep);
  }
  std::sort(deps.begin(), deps.end());
  return deps;
}

bool Resolver::chooseJsr(const std::string &id, const std::vector<Requirement> &reqs,
                         Locked &out) {
  const Source &source = reqs.front().source;
  auto it = versions_.find(id);
  if (it == versions_.end()) {
    JsrVersions versions;
    std::string error;
    if (!jsrVersions(source.scope, source.name, versions, error)) {
      if (error.empty())
        error = "there is no package " + id + " in the registry (" + jsrBaseUrl() + ")";
      report(error);
      return false;
    }
    it = versions_.emplace(id, std::move(versions)).first;
  }
  const std::vector<JsrVersions::Entry> &all = it->second.versions;

  std::vector<Range> ranges;
  for (const Requirement &req : reqs) {
    Range range;
    std::string ignored;
    Range::parse(req.source.range, range, ignored);
    ranges.push_back(std::move(range));
  }
  // The newest version every range takes; with no range at all, the newest
  // one that is not a prerelease, if there is one.
  auto best = [&](const std::vector<const Range *> &rs) -> const Version * {
    const Version *found = nullptr;
    bool anyStable = llvm::any_of(all, [](const JsrVersions::Entry &e) {
      return !e.yanked && !e.version.isPrerelease();
    });
    for (const JsrVersions::Entry &entry : all) {
      if (entry.yanked)
        continue;
      bool ok = llvm::all_of(rs, [&](const Range *r) {
        return r->text().empty() ? !(anyStable && entry.version.isPrerelease())
                                 : r->satisfies(entry.version);
      });
      if (ok && (!found || *found < entry.version))
        found = &entry.version;
    }
    return found;
  };
  std::vector<const Range *> every;
  for (const Range &range : ranges)
    every.push_back(&range);
  const Version *version = best(every);
  if (!version) {
    if (reqs.size() == 1) {
      std::string listed;
      std::vector<Version> available;
      for (const JsrVersions::Entry &entry : all)
        if (!entry.yanked)
          available.push_back(entry.version);
      std::sort(available.begin(), available.end());
      for (size_t i = available.size() > 8 ? available.size() - 8 : 0; i < available.size(); ++i)
        listed += (listed.empty() ? "" : ", ") + available[i].str();
      report(reqs.front().by + " needs " + id + "@" + reqs.front().source.range +
             ", and the registry has no such version" +
             (listed.empty() ? std::string(" (it has none)") : "; it has " + listed));
      return false;
    }
    std::string message = "no one version of " + id + " fits every package that needs it:";
    for (size_t i = 0; i < reqs.size(); ++i) {
      const Version *own = best({&ranges[i]});
      message += "\n  " + reqs[i].by + " wants " +
                 (reqs[i].source.range.empty() ? std::string("any version")
                                               : reqs[i].source.range) +
                 (own ? " (newest: " + own->str() + ")" : std::string(" (there is none)"));
    }
    message += "\na program has each package in one version (docs/packages.md); change the range "
               "in tz.json, or update the package that wants the older one";
    report(message);
    return false;
  }

  out.version = version->str();
  JsrVersion meta;
  std::string error, dir;
  if (!jsrVersion(source.scope, source.name, out.version, meta, error) ||
      !jsrDownload(source.scope, source.name, out.version, meta, dir, error)) {
    report(error);
    return false;
  }
  out.hash = meta.hash;
  out.dependencies = jsrDependencies(dir, meta);
  return true;
}

bool Resolver::chooseGit(const std::string &id, const std::vector<Requirement> &reqs,
                         Locked &out) {
  const Source &source = reqs.front().source;
  for (const Requirement &req : reqs) {
    if (req.source.ref == source.ref)
      continue;
    std::string message = "the packages that need " + id + " want different versions of it:";
    for (const Requirement &r : reqs)
      message += "\n  " + r.by + " wants " +
                 (r.source.ref.empty() ? std::string("its default branch")
                                       : "'" + r.source.ref + "'");
    message += "\na program has each package in one version (docs/packages.md); name the same "
               "tag everywhere";
    report(message);
    return false;
  }
  std::string error, dir;
  out.ref = source.ref;
  if (!gitResolve(source.url, source.ref, updating(id), out.commit, error) ||
      !gitCheckout(source.url, out.commit, dir, error) || !hashDirectory(dir, out.hash, error)) {
    report(error);
    return false;
  }
  Manifest manifest;
  if (!readPackageDir(dir, manifest))
    return false;
  for (const Dependency &dep : manifest.dependencies)
    out.dependencies.push_back(dep.spec);
  return true;
}

bool Resolver::run(Lock &out) {
  for (int round = 0; round < 100; ++round) {
    std::map<std::string, std::vector<Requirement>> reqs;
    if (!collect(reqs))
      return false;
    bool changed = false;
    for (const auto &[id, list] : reqs) {
      if (list.front().source.kind == Source::Kind::Local)
        continue;
      auto current = chosen_.find(id);
      if (current != chosen_.end() && fits(current->second, list))
        continue;
      Locked pick;
      auto old = old_.packages.find(id);
      if (!updating(id) && old != old_.packages.end() && fits(old->second, list))
        pick = old->second;
      else if (!choose(id, list, pick))
        return false;
      chosen_[id] = std::move(pick);
      changed = true;
    }
    for (auto it = chosen_.begin(); it != chosen_.end();) {
      if (reqs.count(it->first)) {
        ++it;
      } else {
        it = chosen_.erase(it);
        changed = true;
      }
    }
    if (!changed) {
      out.packages = chosen_;
      return true;
    }
  }
  report("the versions of the packages do not settle: each choice makes another package need a "
         "different version; pin the versions in tz.json");
  return false;
}

// Whether tz.lock already fits tz.json, and names every package it needs
// and no other: then a build needs no registry. Fills `locals` with the
// local packages reached. A tz.json of a local package that cannot be read
// is reported, and `failed` set.
bool lockFits(const Manifest &root, const Lock &lock, std::map<std::string, Manifest> &locals,
              bool &failed) {
  failed = false;
  std::set<std::string> reached;
  struct Node {
    std::string baseDir;
    std::vector<std::string> specs;
    bool allowLocal;
  };
  std::deque<Node> queue;
  Node first{root.dir, {}, true};
  for (const Dependency &dep : root.dependencies)
    first.specs.push_back(dep.spec);
  queue.push_back(std::move(first));
  while (!queue.empty()) {
    Node node = std::move(queue.front());
    queue.pop_front();
    for (const std::string &spec : node.specs) {
      Source source;
      std::string error;
      if (!parseSource(spec, node.baseDir, source, error))
        return false;
      if (source.kind == Source::Kind::Local && !node.allowLocal)
        return false;
      std::string id = source.id();
      if (source.kind == Source::Kind::Local) {
        if (!reached.insert(id).second)
          continue;
        Manifest local;
        if (!readPackageDir(source.dir, local)) {
          failed = true;
          return false;
        }
        Node next{local.dir, {}, true};
        for (const Dependency &dep : local.dependencies)
          next.specs.push_back(dep.spec);
        queue.push_back(std::move(next));
        locals[id] = std::move(local);
        continue;
      }
      auto it = lock.packages.find(id);
      if (it == lock.packages.end())
        return false;
      const Locked &locked = it->second;
      if (source.kind == Source::Kind::Jsr) {
        Range range;
        Version version;
        if (!Range::parse(source.range, range, error) ||
            !Version::parse(locked.version, version) || !range.satisfies(version))
          return false;
      } else if (source.ref != locked.ref) {
        return false;
      }
      if (reached.insert(id).second)
        queue.push_back({"", locked.dependencies, false});
    }
  }
  // Packages nobody needs any more are dropped from tz.lock.
  for (const auto &entry : lock.packages)
    if (!reached.count(entry.first))
      return false;
  return true;
}

// The sources of a locked package in the cache, checked against tz.lock.
bool ensureSources(const Locked &locked, std::string &dir, JsrVersion *jsrMeta) {
  Source source;
  std::string error;
  llvm::StringRef id = locked.id;
  if (id.starts_with("jsr:")) {
    if (!parseSource(locked.id, "", source, error)) {
      report("tz.lock: " + error);
      return false;
    }
    JsrVersion meta;
    if (!jsrVersion(source.scope, source.name, locked.version, meta, error)) {
      report(error);
      return false;
    }
    if (meta.hash != locked.hash) {
      // A published version never changes: the registry, its mirror or the
      // cache does not hold what it held when tz.lock was written.
      report("the files of " + locked.describe() + " are not those tz.lock names: their hash is " +
             meta.hash + ", tz.lock has " + locked.hash);
      return false;
    }
    if (!jsrDownload(source.scope, source.name, locked.version, meta, dir, error)) {
      report(error);
      return false;
    }
    if (jsrMeta)
      *jsrMeta = std::move(meta);
    return true;
  }
  std::string url = id.drop_front(4).str();
  if (!gitCheckout(url, locked.commit, dir, error)) {
    report(error);
    return false;
  }
  std::string hash;
  if (!hashDirectory(dir, hash, error)) {
    report(error);
    return false;
  }
  if (hash != locked.hash) {
    report("the files of " + locked.describe() + " are not those tz.lock names: their hash is " +
           hash + ", tz.lock has " + locked.hash);
    return false;
  }
  return true;
}

} // namespace

bool preparePackages(const Manifest &manifest, const PrepareOptions &options, Graph &graph,
                     Lock *before, Lock *after) {
  llvm::SmallString<256> lockPath(manifest.dir);
  path::append(lockPath, "tz.lock");
  Lock old;
  if (!readLock(lockPath, old))
    return false;
  if (before)
    *before = old;

  Lock lock;
  std::map<std::string, Manifest> locals;
  bool failed = false;
  if (!options.update && lockFits(manifest, old, locals, failed)) {
    lock = old;
  } else if (failed) {
    return false;
  } else {
    Resolver resolver(manifest, old, options);
    if (!resolver.run(lock))
      return false;
    locals = std::move(resolver.locals);
  }
  std::string text = lockText(lock);
  bool exists = fs::exists(lockPath);
  if (exists || !lock.packages.empty()) {
    std::string current;
    if (auto buffer = llvm::MemoryBuffer::getFile(lockPath, /*IsText=*/true))
      current = (*buffer)->getBuffer().str();
    if (current != text && !writeText(lockPath, text))
      return false;
  }
  if (after)
    *after = lock;

  // The packages of the program, the project first.
  graph = Graph();
  std::map<std::string, unsigned> index;
  std::vector<const Manifest *> manifests; // of each package, null for JSR
  std::vector<Manifest> fetched;
  fetched.reserve(lock.packages.size());
  llvm::StringSet<> names;

  Package project;
  project.dir = manifest.dir;
  project.manifestPath = manifest.path;
  project.main = manifest.main;
  project.exports = manifest.exports;
  graph.packages.push_back(std::move(project));
  manifests.push_back(&manifest);

  auto uniqueName = [&](std::string name) {
    std::string unique = name;
    for (int n = 2; !names.insert(unique).second; ++n)
      unique = name + "~" + std::to_string(n);
    return unique;
  };
  for (const auto &[id, local] : locals) {
    Package p;
    p.id = id;
    p.dir = local.dir;
    p.name = uniqueName(local.name.empty() ? path::filename(local.dir).str() : local.name);
    p.version = local.version;
    p.manifestPath = local.path;
    p.main = local.main;
    p.exports = local.exports;
    index[id] = static_cast<unsigned>(graph.packages.size());
    graph.packages.push_back(std::move(p));
    manifests.push_back(&local);
  }
  for (const auto &[id, locked] : lock.packages) {
    std::string dir;
    JsrVersion meta;
    bool jsr = llvm::StringRef(id).starts_with("jsr:");
    if (!ensureSources(locked, dir, jsr ? &meta : nullptr))
      return false;
    Package p;
    p.id = id;
    p.dir = canonical(dir);
    p.external = true;
    p.jsr = jsr;
    p.version = jsr ? locked.version : locked.ref;
    std::string error;
    if (jsr) {
      p.name = uniqueName(id.substr(4));
      p.exports = meta.exports;
      for (const std::string &dep : locked.dependencies) {
        Source target;
        if (parseSource(dep, "", target, error))
          if (lock.packages.count(target.id()))
            p.jsrDeps["@" + target.scope + "/" + target.name] = 0; // the index is set below
      }
      manifests.push_back(nullptr);
    } else {
      fetched.emplace_back();
      if (!readPackageDir(dir, fetched.back()))
        return false;
      const Manifest &m = fetched.back();
      p.name = uniqueName(m.name.empty() ? repositoryName(id.substr(4)) : m.name);
      p.manifestPath = m.path;
      p.main = m.main;
      p.exports = m.exports;
      manifests.push_back(&fetched.back());
    }
    index[id] = static_cast<unsigned>(graph.packages.size());
    graph.packages.push_back(std::move(p));
  }
  // Which package each import name means.
  for (size_t i = 0; i < graph.packages.size(); ++i) {
    Package &p = graph.packages[i];
    if (const Manifest *m = manifests[i]) {
      for (const Dependency &dep : m->dependencies) {
        Source target;
        std::string error;
        if (parseSource(dep.spec, m->dir, target, error))
          if (auto it = index.find(target.id()); it != index.end())
            p.aliases[dep.alias] = it->second;
      }
    }
    for (auto &entry : p.jsrDeps)
      if (auto it = index.find("jsr:" + entry.getKey().str()); it != index.end())
        entry.second = it->second;
  }
  return true;
}

} // namespace tz::pkg
