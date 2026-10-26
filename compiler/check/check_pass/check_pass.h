#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

class CheckPass {
public:
  Errors run(Program &program) const;
};

}
