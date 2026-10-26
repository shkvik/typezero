#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
#include "compiler/parallel/explainer/explainer.h"

namespace tz {

ParallelPlan planParallel(const SourceFile &program, const llvm::SourceMgr &sources) {
  parallel::Analysis analysis(program, sources);
  analysis.run();

  return analysis.plan();
}

void explainParallel(const SourceFile &program, const llvm::SourceMgr &sources,
                     llvm::raw_ostream &out) {
  parallel::Analysis analysis(program, sources);
  analysis.run();
  analysis.explainer().print(out);
}

}
