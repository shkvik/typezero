#include "compiler/cli/command_line/project_file/project_file.h"
#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/graph/graph.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/package_cache/package_cache.h"
#include "compiler/packages/package_graph/package_graph.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "compiler/packages/report/report.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

bool ProjectFile::writeText(llvm::StringRef file, llvm::StringRef text) {
  std::string error;
  if (PackageCache::writeFileAtomically(file, text, error)) {
    return true;
  }
  Report::report(error);

  return false;
}

bool ProjectFile::currentProject(Manifest &out) {
  std::string file = ManifestFile::findManifest("");
  if (file.empty()) {
    Report::report(
        "there is no package.json in this directory or above it; make one with 'tz init'");
    return false;
  }

  return ManifestFile::readManifest(file, out);
}

bool ProjectFile::applyDependencies(const Manifest &project, const std::vector<Dependency> &deps,
                                    const PrepareOptions &options, Lock *before, Lock *after) {
  std::string text = ManifestFile::withDependencies(project, deps);
  Manifest edited;
  if (!ManifestFile::readManifestText(project.path, text, edited)) {
    return false;
  }
  Graph graph;
  if (!PackageGraph::preparePackages(edited, options, graph, before, after)) {
    return false;
  }

  return text == project.text || ProjectFile::writeText(project.path, text);
}

}
