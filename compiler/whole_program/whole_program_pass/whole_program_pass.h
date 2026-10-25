#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

// What only the whole program shows: the order files load in, what is unused, the numbers of
// classes, the globals without a type, writes through a wider type.
class WholeProgramPass {
public:
  Errors run(Program &program) const;
};

} // namespace tz
