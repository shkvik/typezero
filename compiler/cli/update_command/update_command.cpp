#include "compiler/cli/update_command/update_command.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/cli/project_file/project_file.h"
#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/graph/graph.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/package_graph/package_graph.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/source/source.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace {

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
                   << " -> " << (now.version.empty() ? now.commit.substr(0, 12) : now.version)
                   << "\n";
      any = true;
    }
  }
  for (const auto &[id, old] : before.packages) {
    if (!after.packages.count(id)) {
      llvm::outs() << "removed " << old.describe() << "\n";
      any = true;
    }
  }
  if (!any) {
    llvm::outs() << "the packages are up to date\n";
  }
}

}

int UpdateCommand::run(llvm::ArrayRef<const char *> args) const {
  Manifest project;
  if (!currentProject(project)) {
    return kExitError;
  }
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
  if (!preparePackages(project, options, graph, &before, &after)) {
    return kExitError;
  }
  printChanges(before, after);

  return 0;
}

}
