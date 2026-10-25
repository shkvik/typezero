#pragma once

#include "compiler/parallel/parallel_analysis/parallel_analysis.h"

namespace tz {

struct Program;

// What of the async code may run at the same time as other (docs/async.md, section 8). With
// TZ_NO_PARALLEL (for the developers of tz, and benchmarks) the program runs on one thread, with
// the runtime of one.
class ParallelPass {
public:
  ParallelPlan run(const Program &program) const;
};

} // namespace tz
