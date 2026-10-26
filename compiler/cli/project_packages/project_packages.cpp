#include "compiler/cli/project_packages/project_packages.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/package_graph/package_graph.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace tz {

bool ProjectPackages::prepare(llvm::StringRef input) {
  graph_ = nullptr;
  llvm::SmallString<256> dir(input);
  llvm::sys::fs::make_absolute(dir);
  llvm::sys::path::remove_filename(dir);
  std::string manifestPath = pkg::findManifest(dir);
  if (manifestPath.empty()) {
    return true;
  }
  pkg::Manifest manifest;
  if (!pkg::readManifest(manifestPath, manifest) ||
      !pkg::preparePackages(manifest, pkg::PrepareOptions(), storage_)) {
    return false;
  }
  graph_ = &storage_;

  return true;
}

}
