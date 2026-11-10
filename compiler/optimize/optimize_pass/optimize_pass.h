#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "llvm/IR/Module.h"
#include <vector>

namespace tz {

class Target;
struct BuildOptions;

class OptimizePass {
public:
  OptimizePass(const Target &target, const BuildOptions &options);

  std::vector<Diagnostic> run(llvm::Module &module, bool parallel) const;

private:
  void runPasses(llvm::Module &module) const;

  const Target &target;
  const BuildOptions &options;
};

}
