#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

// The return types of functions that call each other without a type: their cycles, inferred
// together.
class InferPass {
public:
  Errors run(Program &program) const;
};

} // namespace tz
