#pragma once

#include "compiler/packages/graph/graph.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/prepare_options/prepare_options.h"

namespace tz::pkg {

bool preparePackages(const Manifest &manifest, const PrepareOptions &options, Graph &graph,
                     Lock *before = nullptr, Lock *after = nullptr);

}
