#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct Program;

class InstancesPass {
public:
  Errors run(Program &program) const;
};

}
