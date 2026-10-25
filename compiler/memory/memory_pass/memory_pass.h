#pragma once

#include "compiler/memory/memory_plan/memory_plan.h"

namespace tz {

struct Program;

// Which objects of the program are members of groups, and what is proved of the others
// (docs/objects.md, section 11): the plan lowering counts references by.
class MemoryPass {
public:
  MemoryPlan run(Program &program) const;
};

} // namespace tz
