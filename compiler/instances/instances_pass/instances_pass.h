#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

// The copies of generics that calls need and the classes that class expressions made, now
// that every generic body is checked.
class InstancesPass {
public:
  Errors run(Program &program) const;
};

} // namespace tz
