#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class Args;

class BuildCommand {
public:
  explicit BuildCommand(const RuntimeFiles &runtime);

  int run(const Args &args, llvm::StringRef output) const;

private:
  const RuntimeFiles &runtime_;
};

}
