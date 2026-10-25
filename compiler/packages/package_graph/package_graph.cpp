#include "compiler/packages/package_graph/package_graph.h"
#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/git_client/git_client.h"
#include "compiler/packages/graph/graph.h"
#include "compiler/packages/jsr_client/jsr_client.h"
#include "compiler/packages/jsr_version/jsr_version.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/locked/locked.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/package/package.h"
#include "compiler/packages/package_cache/package_cache.h"
#include "compiler/packages/paths/paths.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "compiler/packages/range/range.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/resolver/resolver.h"
#include "compiler/packages/scanner/scanner.h"
#include "compiler/packages/source/source.h"
#include "compiler/packages/specifier/specifier.h"
#include "compiler/packages/version/version.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <deque>
#include <set>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

bool writeText(llvm::StringRef file, llvm::StringRef text) {
  std::string error;
  if (writeFileAtomically(file, text, error)) {
    return true;
  }
  report(error);

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
  for (const Dependency &dep : root.dependencies) {
    first.specs.push_back(dep.spec);
  }
  queue.push_back(std::move(first));
  while (!queue.empty()) {
    Node node = std::move(queue.front());
    queue.pop_front();
    for (const std::string &spec : node.specs) {
      Source source;
      std::string error;
      if (!parseSource(spec, node.baseDir, source, error)) {
        return false;
      }
      if (source.kind == Source::Kind::Local && !node.allowLocal) {
        return false;
      }
      std::string id = source.id();
      if (source.kind == Source::Kind::Local) {
        if (!reached.insert(id).second) {
          continue;
        }
        Manifest local;
        if (!readPackageDir(source.dir, local)) {
          failed = true;
          return false;
        }
        Node next{local.dir, {}, true};
        for (const Dependency &dep : local.dependencies) {
          next.specs.push_back(dep.spec);
        }
        queue.push_back(std::move(next));
        locals[id] = std::move(local);
        continue;
      }
      auto it = lock.packages.find(id);
      if (it == lock.packages.end()) {
        return false;
      }
      const Locked &locked = it->second;
      if (source.kind == Source::Kind::Jsr) {
        Range range;
        Version version;
        if (!Range::parse(source.range, range, error) || !Version::parse(locked.version, version) ||
            !range.satisfies(version)) {
          return false;
        }
      } else if (source.ref != locked.ref) {
        return false;
      }
      if (reached.insert(id).second) {
        queue.push_back({"", locked.dependencies, false});
      }
    }
  }
  // Packages nobody needs any more are dropped from tz.lock.
  for (const auto &entry : lock.packages) {
    if (!reached.count(entry.first)) {
      return false;
    }
  }

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
    if (jsrMeta) {
      *jsrMeta = std::move(meta);
    }
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
  if (!readLock(lockPath, old)) {
    return false;
  }
  if (before) {
    *before = old;
  }

  Lock lock;
  std::map<std::string, Manifest> locals;
  bool failed = false;
  if (!options.update && lockFits(manifest, old, locals, failed)) {
    lock = old;
  } else if (failed) {
    return false;
  } else {
    Resolver resolver(manifest, old, options);
    if (!resolver.run(lock)) {
      return false;
    }
    locals = std::move(resolver.locals);
  }
  std::string text = lockText(lock);
  bool exists = fs::exists(lockPath);
  if (exists || !lock.packages.empty()) {
    std::string current;
    if (auto buffer = llvm::MemoryBuffer::getFile(lockPath, /*IsText=*/true)) {
      current = (*buffer)->getBuffer().str();
    }
    if (current != text && !writeText(lockPath, text)) {
      return false;
    }
  }
  if (after) {
    *after = lock;
  }

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
    for (int n = 2; !names.insert(unique).second; ++n) {
      unique = name + "~" + std::to_string(n);
    }

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
    if (!ensureSources(locked, dir, jsr ? &meta : nullptr)) {
      return false;
    }
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
        if (parseSource(dep, "", target, error)) {
          if (lock.packages.count(target.id())) {
            p.jsrDeps["@" + target.scope + "/" + target.name] = 0; // the index is set below
          }
        }
      }
      manifests.push_back(nullptr);
    } else {
      fetched.emplace_back();
      if (!readPackageDir(dir, fetched.back())) {
        return false;
      }
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
        if (parseSource(dep.spec, m->dir, target, error)) {
          if (auto it = index.find(target.id()); it != index.end()) {
            p.aliases[dep.alias] = it->second;
          }
        }
      }
    }
    for (auto &entry : p.jsrDeps) {
      if (auto it = index.find("jsr:" + entry.getKey().str()); it != index.end()) {
        entry.second = it->second;
      }
    }
  }

  return true;
}

} // namespace tz::pkg
