#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

// Warnings where an int may overflow (docs/small.md, section 6); they do not stop the build.
class OverflowPass {
public:
  Errors run(Program &program) const;
};

} // namespace tz
