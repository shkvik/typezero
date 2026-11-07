#pragma once

#include "compiler/packages/graph/graph.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/prepare_options/prepare_options.h"

namespace tz::pkg {

struct JsrVersion;

class PackageGraph {
public:
  static bool preparePackages(const Manifest &manifest, const PrepareOptions &options, Graph &graph,
                              Lock *before = nullptr, Lock *after = nullptr);

private:
  static bool writeText(llvm::StringRef file, llvm::StringRef text);
  static bool lockFits(const Manifest &root, const Lock &lock,
                       std::map<std::string, Manifest> &locals, bool &failed);
  static bool ensureSources(const Locked &locked, std::string &dir, JsrVersion *jsrMeta);
};

}
