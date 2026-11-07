#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct BuildOptions;
struct Program;

Errors buildProgram(Program &program, const BuildOptions &options);

}
