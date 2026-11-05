#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/parallel/analysis/analysis.h"

namespace tz {

ParallelPlan planParallel(const SourceFile &program, const llvm::SourceMgr &sources,
                          const AstContext &ast) {
  parallel::Analysis analysis(program, sources, ast);
  analysis.run();

  return analysis.plan();
}

void explainParallel(const SourceFile &program, const llvm::SourceMgr &sources,
                     const AstContext &ast, llvm::raw_ostream &out) {
  parallel::Analysis analysis(program, sources, ast);
  analysis.run();
  analysis.explain(out);
}

}
