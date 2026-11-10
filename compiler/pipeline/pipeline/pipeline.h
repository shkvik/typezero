#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

struct BuildOptions;
struct Program;

struct BuiltProgram {
  std::vector<Diagnostic> warnings;
};

using ResultBuiltProgram = Result<BuiltProgram, std::vector<Diagnostic>>;

ResultBuiltProgram buildProgram(Program &program, const BuildOptions &options);

}
