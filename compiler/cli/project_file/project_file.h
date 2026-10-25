#pragma once

#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz::pkg {

// Writes `text` to `file`, reporting why it cannot.

bool writeText(llvm::StringRef file, llvm::StringRef text);

// The project of the current directory.
bool currentProject(Manifest &out);

// Prepares the packages of the project as tz.json would be with `deps`, and
// writes tz.json if that works.
bool applyDependencies(const Manifest &project, const std::vector<Dependency> &deps,
                       const PrepareOptions &options, Lock *before, Lock *after);

} // namespace tz::pkg
