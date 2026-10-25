#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"

namespace tz {

class Args;

// `tz build --explain-parallel`: the program checked, as a build checks it, then what of it may
// run in parallel and why (docs/async.md, section 5), on stdout. Nothing is built.
class ExplainCommand {
public:
  explicit ExplainCommand(const RuntimeFiles &runtime);

  int run(const Args &args) const;

private:
  const RuntimeFiles &runtime_;
};

} // namespace tz
