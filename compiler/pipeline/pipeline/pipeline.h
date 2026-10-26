#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct BuildOptions;
struct Program;

Errors checkProgram(Program &program, const BuildOptions &options);
Errors buildProgram(Program &program, const BuildOptions &options);

}
