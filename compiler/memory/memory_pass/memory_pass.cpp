#include "compiler/memory/memory_pass/memory_pass.h"
#include "compiler/memory/memory_analysis/memory_analysis.h"
#include "compiler/program/program/program.h"

namespace tz {

MemoryPlan MemoryPass::run(Program &program) const {
  MemoryPlan plan;
  plan.subtypes = program.file.subtypes;
  MemoryAnalysis analysis(plan);
  analysis.analyzeCycles(program.file);

  return plan;
}

} // namespace tz
