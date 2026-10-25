#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"

namespace tz {

class Args;

// `tz licenses app.exe`: the licenses of the code of others the executable embeds.
class LicensesCommand {
public:
  explicit LicensesCommand(const RuntimeFiles &runtime);

  int run(const Args &args) const;

private:
  const RuntimeFiles &runtime_;
};

} // namespace tz
