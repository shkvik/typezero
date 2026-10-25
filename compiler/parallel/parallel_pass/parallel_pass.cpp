#include "compiler/parallel/parallel_pass/parallel_pass.h"
#include "compiler/program/program/program.h"
#include <cstdlib>

namespace tz {

ParallelPlan ParallelPass::run(const Program &program) const {
  if (!program.file.usesAsync || std::getenv("TZ_NO_PARALLEL")) {
    return ParallelPlan();
  }

  return planParallel(program.file, program.sourceMgr);
}

} // namespace tz
