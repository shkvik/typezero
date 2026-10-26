#pragma once

#include "compiler/parallel/parallel_analysis/parallel_analysis.h"

namespace tz {

struct Program;

class ParallelPass {
public:
  ParallelPlan run(const Program &program) const;
};

}
