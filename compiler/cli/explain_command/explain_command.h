#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"

namespace tz {

class Args;

class ExplainCommand {
public:
  explicit ExplainCommand(const RuntimeFiles &runtime);

  int run(const Args &args) const;

private:
  const RuntimeFiles &runtime_;
};

}
