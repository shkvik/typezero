#pragma once

#include "compiler/packages/graph/graph.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/prepare_options/prepare_options.h"

namespace tz::pkg {

// Makes tz.lock fit `manifest` (writing it if it changes), puts the sources of
// every package into the cache and describes them in `graph`. `before` and
// `after` get tz.lock as it was and as it is now. Reports errors.
bool preparePackages(const Manifest &manifest, const PrepareOptions &options, Graph &graph,
                     Lock *before = nullptr, Lock *after = nullptr);

} // namespace tz::pkg
