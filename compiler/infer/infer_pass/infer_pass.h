#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

class InferPass {
public:
  Errors run(Program &program) const;
};

}
