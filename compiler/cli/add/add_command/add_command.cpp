#include "compiler/cli/add/add_command/add_command.h"
#include "compiler/cli/command_line/command_usage/command_usage.h"
#include "compiler/cli/command_line/exit_codes/exit_codes.h"
#include "compiler/cli/command_line/project_file/project_file.h"
#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/git_client/git_client.h"
#include "compiler/packages/jsr_client/jsr_client.h"
#include "compiler/packages/jsr_versions/jsr_versions.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/source/source.h"
#include "compiler/packages/specifier/specifier.h"
#include "compiler/packages/version/version.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace path = llvm::sys::path;

std::string AddCommand::relativePath(llvm::StringRef from, llvm::StringRef to) {
  std::vector<llvm::StringRef> base(path::begin(from), path::end(from));
  std::vector<llvm::StringRef> target(path::begin(to), path::end(to));
  size_t common = 0;
  while (common < base.size() && common < target.size() &&
         base[common].equals_insensitive(target[common])) {
    ++common;
  }
  if (common == 0) {
    return path::convert_to_slash(to);
  }
  std::string result;
  for (size_t i = common; i < base.size(); ++i) {
    result += "../";
  }
  for (size_t i = common; i < target.size(); ++i) {
    result += target[i].str() + "/";
  }
  if (result.empty()) {
    return ".";
  }
  result.pop_back();

  return result.starts_with("../") ? result : "./" + result;
}

bool AddCommand::isAbsolutePath(llvm::StringRef text) {
  return text.starts_with("/") || text.starts_with("\\") ||
         (text.size() > 2 && llvm::isAlpha(text[0]) && text[1] == ':' &&
          (text[2] == '/' || text[2] == '\\'));
}

bool AddCommand::interpretAdd(const Manifest &project, llvm::StringRef arg, Dependency &out) {
  std::string error;
  Source source;
  llvm::StringRef text = arg;
  bool bare = !text.starts_with("jsr:") && !text.starts_with("@") && !text.contains(':') &&
              !text.starts_with(".") && !AddCommand::isAbsolutePath(text);
  if (bare || text.starts_with("@") || text.starts_with("jsr:")) {
    std::string spec;
    if (bare) {
      auto [name, range] = text.split('@');
      spec = "jsr:@tz/" + name.str() + (range.empty() ? "" : "@" + range.str());
    } else {
      spec = text.starts_with("jsr:") ? text.str() : "jsr:" + text.str();
    }
    if (!SourceParser::parseSource(spec, project.dir, source, error)) {
      Report::report(error);
      return false;
    }
    out.alias = source.scope() == "tz" ? source.name() : "@" + source.scope() + "/" + source.name();
    if (source.range().empty()) {
      JsrVersions versions;
      if (!JsrClient::jsrVersions(source.scope(), source.name(), versions, error)) {
        if (!error.empty()) {
          Report::report(error);
        } else if (bare) {
          Report::report("'" + source.name() + "' has not been adapted for tz yet: there is no " +
                         source.id() +
                         " in the registry; give its source, e.g. 'tz add "
                         "github:user/repo@v1.0.0', or adapt it with 'tz port'");
        } else {
          Report::report("there is no package " + source.id() + " in the registry (" +
                         JsrClient::jsrBaseUrl() + ")");
        }
        return false;
      }
      const Version *newest = nullptr, *newestPre = nullptr;
      for (const JsrVersions::Entry &entry : versions.versions) {
        if (entry.yanked) {
          continue;
        }
        const Version *&slot = entry.version.isPrerelease() ? newestPre : newest;
        if (!slot || *slot < entry.version) {
          slot = &entry.version;
        }
      }
      if (!newest) {
        newest = newestPre;
      }
      if (!newest) {
        Report::report(source.id() + " has no versions");
        return false;
      }
      spec += "@^" + newest->str();
    }
    out.spec = spec;
    return DependencyAlias::checkAlias(out.alias, error) || (Report::report(error), false);
  }

  std::string spec = AddCommand::isAbsolutePath(text) ? "file:" + text.str() : text.str();
  if (!SourceParser::parseSource(spec, project.dir, source, error)) {
    Report::report(error);
    return false;
  }
  Manifest package;
  if (source.form.is<LocalSource>()) {
    if (!ManifestFile::readPackageDir(source.dir(), package)) {
      return false;
    }
    out.spec = "file:" + AddCommand::relativePath(project.dir, source.dir());
  } else {
    std::string commit, dir;
    if (!GitClient::gitResolve(source.url(), source.ref(), true, commit, error) ||
        !GitClient::gitCheckout(source.url(), commit, dir, error)) {
      Report::report(error);
      return false;
    }
    if (!ManifestFile::readPackageDir(dir, package)) {
      return false;
    }
    out.spec = text.str();
  }
  out.alias = package.name.empty() ? source.shortName() : package.name;
  if (!DependencyAlias::checkAlias(out.alias, error)) {
    Report::report(error +
                   "; add the package to package.json by hand, under the name its imports use");
    return false;
  }

  return true;
}

int AddCommand::run(llvm::ArrayRef<const char *> args) const {
  const char *synopsis = "tz add <package>...\n\n"
                         "  tz add zod                     zod adapted for tz: jsr:@tz/zod\n"
                         "  tz add @std/path@^1            a JSR package\n"
                         "  tz add github:user/repo@v1.0   a GitHub repository at a tag\n"
                         "  tz add git:<address>@v1.0      any git repository\n"
                         "  tz add ../lib                  a directory";
  if (args.empty()) {
    return CommandUsage::usage("'tz add' needs a package", synopsis);
  }
  for (llvm::StringRef arg : args) {
    if (arg.starts_with("-")) {
      return CommandUsage::usage("unknown option '" + arg + "'", synopsis);
    }
  }
  Manifest project;
  if (!ProjectFile::currentProject(project)) {
    return kExitError;
  }
  std::vector<Dependency> deps = project.dependencies;
  PrepareOptions options;
  options.update = true;
  std::vector<Dependency> added;
  for (llvm::StringRef arg : args) {
    Dependency dep;
    if (!AddCommand::interpretAdd(project, arg, dep)) {
      return kExitError;
    }
    auto it = std::find_if(deps.begin(), deps.end(),
                           [&](const Dependency &d) { return d.alias == dep.alias; });
    if (it != deps.end()) {
      it->spec = dep.spec;
    } else {
      deps.push_back(dep);
    }
    Source source;
    std::string error;
    if (SourceParser::parseSource(dep.spec, project.dir, source, error)) {
      options.only.push_back(source.id());
    }
    added.push_back(dep);
  }
  Lock before, after;
  if (!ProjectFile::applyDependencies(project, deps, options, &before, &after)) {
    return kExitError;
  }
  for (const Dependency &dep : added) {
    llvm::outs() << "added " << dep.alias << ": " << dep.spec;
    Source source;
    std::string error;
    if (SourceParser::parseSource(dep.spec, project.dir, source, error)) {
      if (auto it = after.packages.find(source.id()); it != after.packages.end()) {
        llvm::outs() << " ("
                     << (it->second.version.empty() ? it->second.commit.substr(0, 12)
                                                    : it->second.version)
                     << ")";
      }
    }
    llvm::outs() << "\n";
  }

  return 0;
}

}
