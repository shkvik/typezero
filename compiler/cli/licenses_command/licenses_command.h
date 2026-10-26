#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"

namespace tz {

class Args;

class LicensesCommand {
public:
  explicit LicensesCommand(const RuntimeFiles &runtime);

  int run(const Args &args) const;

private:
  const RuntimeFiles &runtime_;
};

}
