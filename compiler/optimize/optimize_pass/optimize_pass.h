#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/IR/Module.h"

namespace tz {

class Target;
struct BuildOptions;

// The hot functions of the runtime copied into the program, then LLVM's optimizations; with -O0
// neither.
class OptimizePass {
public:
  OptimizePass(const Target &target, const BuildOptions &options);

  Errors run(llvm::Module &module, bool parallel) const;

private:
  void runPasses(llvm::Module &module) const;

  const Target &target_;
  const BuildOptions &options_;
};

} // namespace tz
