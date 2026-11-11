#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

struct BuildOptions;
struct Program;

using ResultCheckedProgram = Result<void, std::vector<Diagnostic>>;

ResultCheckedProgram checkProgram(Program &program, const BuildOptions &options);

}
