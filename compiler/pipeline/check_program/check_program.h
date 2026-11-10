#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

struct BuildOptions;
struct Program;

struct CheckedProgram {
  std::vector<Diagnostic> warnings;
};

using ResultCheckedProgram = Result<CheckedProgram, std::vector<Diagnostic>>;

ResultCheckedProgram checkProgram(Program &program, const BuildOptions &options);

}
