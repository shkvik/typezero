#pragma once

#include "compiler/memory/memory_plan/memory_plan.h"

namespace tz {

struct Program;

class MemoryPass {
public:
  MemoryPlan run(Program &program) const;
};

}
