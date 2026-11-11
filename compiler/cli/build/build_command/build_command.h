#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

class Args;
struct BuildOptions;
struct Program;

class BuildCommand {
public:
  explicit BuildCommand(const RuntimeFiles &runtime);

  int run(const Args &args, llvm::StringRef output) const;
  int explain(const Args &args) const;

private:
  struct BuiltProgram {
    std::vector<Diagnostic> warnings;
  };

  using ResultBuiltProgram = Result<BuiltProgram, std::vector<Diagnostic>>;

  ResultBuiltProgram buildProgram(Program &program, const BuildOptions &options) const;

  const RuntimeFiles &runtime;
};

}
