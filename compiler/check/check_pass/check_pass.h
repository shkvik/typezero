#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

// The top-level code of every file, in the order the files load, then the bodies of functions
// and classes: the type of each expression, and whose each call is.
class CheckPass {
public:
  Errors run(Program &program) const;
};

} // namespace tz
