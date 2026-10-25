#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"

namespace tz {

class Args;

// `tz run`: the program built into a temporary executable and run with the arguments after the
// file; tz exits with its code.
class RunCommand {
public:
  explicit RunCommand(const RuntimeFiles &runtime);

  int run(const Args &args) const;

private:
  const RuntimeFiles &runtime_;
};

} // namespace tz
