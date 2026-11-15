#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "llvm/ADT/ArrayRef.h"
#include <string>

namespace tz {

class Args;

class RunCommand {
public:
  explicit RunCommand(const RuntimeFiles &runtime);

  int run(const Args &args) const;

private:
  void addSearchPath(llvm::ArrayRef<std::string> dirs) const;

  const RuntimeFiles &runtime;
};

}
