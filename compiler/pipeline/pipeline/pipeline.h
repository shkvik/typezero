#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct BuildOptions;
struct Program;
class PassTimes;

Errors checkProgram(Program &program, const BuildOptions &options, PassTimes *times = nullptr);
Errors buildProgram(Program &program, const BuildOptions &options);

}
