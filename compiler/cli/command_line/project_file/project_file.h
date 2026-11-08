#pragma once

#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz::pkg {

class ProjectFile {
public:
  static bool currentProject(Manifest &out);
  static bool writeText(llvm::StringRef file, llvm::StringRef text);
  static bool applyDependencies(const Manifest &project, const std::vector<Dependency> &deps,
                                const PrepareOptions &options, Lock *before, Lock *after);
};

}
