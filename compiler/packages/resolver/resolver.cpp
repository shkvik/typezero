#include "compiler/packages/resolver/resolver.h"
#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/git_client/git_client.h"
#include "compiler/packages/jsr_client/jsr_client.h"
#include "compiler/packages/jsr_version/jsr_version.h"
#include "compiler/packages/jsr_versions/jsr_versions.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/locked/locked.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/package_cache/package_cache.h"
#include "compiler/packages/range/range.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/requirement/requirement.h"
#include "compiler/packages/scanner/scanner.h"
#include "compiler/packages/source/source.h"
#include "compiler/packages/specifier/specifier.h"
#include "compiler/packages/version/version.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <deque>
#include <set>

namespace tz::pkg {

namespace path = llvm::sys::path;

namespace {

std::vector<std::string> jsrDependencies(const std::string &dir, const JsrVersion &meta) {
  std::vector<std::string> entries, files, external, deps;
  for (const auto &[name, file] : meta.exports) {
    llvm::SmallString<256> full(dir);
    path::append(full, file);
    entries.push_back(std::string(full));
  }
  reachableFiles(entries, files, external, Sources::Tz);
  for (llvm::StringRef spec : external) {
    std::string package, dep, subpath;
    if (splitJsrImport(spec, package, dep, subpath) &&
        std::find(deps.begin(), deps.end(), dep) == deps.end()) {
      deps.push_back(dep);
    }
  }
  std::sort(deps.begin(), deps.end());

  return deps;
}

}

void Resolver::reportSpec(const std::string &by, const Dependency *dep,
                          const std::string &message) {
  const Manifest *manifest = nullptr;
  if (by == "tz.json") {
    manifest = &root_;
  }
  for (const auto &[id, local] : locals) {
    if (by == local.path) {
      manifest = &local;
    }
  }
  if (manifest && dep) {
    reportAt(manifest->path, manifest->text, dep->offset, message);
  } else {
    report(by + ": " + message);
  }
}

bool Resolver::collect(std::map<std::string, std::vector<Requirement>> &reqs) {
  reqs.clear();
  struct Node {
    std::string by;
    std::string baseDir;
    bool allowLocal;
    const std::vector<Dependency> *deps;
    const std::vector<std::string> *specs;
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
      if (!visited.insert(id).second) {
        continue;
      }
      if (source.kind == Source::Kind::Local) {
        auto it = locals.find(id);
        if (it == locals.end()) {
          Manifest local;
          if (!readPackageDir(source.dir, local)) {
            return false;
          }
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
          !Version::parse(locked.version, version) || !range.satisfies(version)) {
        return false;
      }
    } else if (req.source.kind == Source::Kind::Git) {
      if (req.source.ref != locked.ref) {
        return false;
      }
    }
  }

  return true;
}

bool Resolver::choose(const std::string &id, const std::vector<Requirement> &reqs, Locked &out) {
  out = Locked();
  out.id = id;
  if (reqs.front().source.kind == Source::Kind::Jsr) {
    return chooseJsr(id, reqs, out);
  }

  return chooseGit(id, reqs, out);
}

bool Resolver::chooseJsr(const std::string &id, const std::vector<Requirement> &reqs, Locked &out) {
  const Source &source = reqs.front().source;
  auto it = versions_.find(id);
  if (it == versions_.end()) {
    JsrVersions versions;
    std::string error;
    if (!jsrVersions(source.scope, source.name, versions, error)) {
      if (error.empty()) {
        error = "there is no package " + id + " in the registry (" + jsrBaseUrl() + ")";
      }
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
  auto best = [&](const std::vector<const Range *> &rs) -> const Version * {
    const Version *found = nullptr;
    bool anyStable = llvm::any_of(
        all, [](const JsrVersions::Entry &e) { return !e.yanked && !e.version.isPrerelease(); });
    for (const JsrVersions::Entry &entry : all) {
      if (entry.yanked) {
        continue;
      }
      bool ok = llvm::all_of(rs, [&](const Range *r) {
        return r->text().empty() ? !(anyStable && entry.version.isPrerelease())
                                 : r->satisfies(entry.version);
      });
      if (ok && (!found || *found < entry.version)) {
        found = &entry.version;
      }
    }

    return found;
  };
  std::vector<const Range *> every;
  for (const Range &range : ranges) {
    every.push_back(&range);
  }
  const Version *version = best(every);
  if (!version) {
    if (reqs.size() == 1) {
      std::string listed;
      std::vector<Version> available;
      for (const JsrVersions::Entry &entry : all) {
        if (!entry.yanked) {
          available.push_back(entry.version);
        }
      }
      std::sort(available.begin(), available.end());
      for (size_t i = available.size() > 8 ? available.size() - 8 : 0; i < available.size(); ++i) {
        listed += (listed.empty() ? "" : ", ") + available[i].str();
      }
      report(reqs.front().by + " needs " + id + "@" + reqs.front().source.range +
             ", and the registry has no such version" +
             (listed.empty() ? std::string(" (it has none)") : "; it has " + listed));
      return false;
    }
    std::string message = "no one version of " + id + " fits every package that needs it:";
    for (size_t i = 0; i < reqs.size(); ++i) {
      const Version *own = best({&ranges[i]});
      message +=
          "\n  " + reqs[i].by + " wants " +
          (reqs[i].source.range.empty() ? std::string("any version") : reqs[i].source.range) +
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

bool Resolver::chooseGit(const std::string &id, const std::vector<Requirement> &reqs, Locked &out) {
  const Source &source = reqs.front().source;
  for (const Requirement &req : reqs) {
    if (req.source.ref == source.ref) {
      continue;
    }
    std::string message = "the packages that need " + id + " want different versions of it:";
    for (const Requirement &r : reqs) {
      message +=
          "\n  " + r.by + " wants " +
          (r.source.ref.empty() ? std::string("its default branch") : "'" + r.source.ref + "'");
    }
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
  if (!readPackageDir(dir, manifest)) {
    return false;
  }
  for (const Dependency &dep : manifest.dependencies) {
    out.dependencies.push_back(dep.spec);
  }

  return true;
}

bool Resolver::run(Lock &out) {
  for (int round = 0; round < 100; ++round) {
    std::map<std::string, std::vector<Requirement>> reqs;
    if (!collect(reqs)) {
      return false;
    }
    bool changed = false;
    for (const auto &[id, list] : reqs) {
      if (list.front().source.kind == Source::Kind::Local) {
        continue;
      }
      auto current = chosen_.find(id);
      if (current != chosen_.end() && fits(current->second, list)) {
        continue;
      }
      Locked pick;
      auto old = old_.packages.find(id);
      if (!updating(id) && old != old_.packages.end() && fits(old->second, list)) {
        pick = old->second;
      } else if (!choose(id, list, pick)) {
        return false;
      }
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

}
