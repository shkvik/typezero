#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct BuildOptions;
struct Program;

// The way from the entry file to an executable. Each returns all that its passes found; in an if,
// the Errors are true when there is an error among them.
Errors checkProgram(Program &program, const BuildOptions &options);
Errors buildProgram(Program &program, const BuildOptions &options);

} // namespace tz
