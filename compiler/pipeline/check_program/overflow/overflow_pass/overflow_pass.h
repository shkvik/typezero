#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

class OverflowPass {
public:
  Errors run(Program &program) const;
};

}
