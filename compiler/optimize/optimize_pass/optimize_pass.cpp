#include "compiler/optimize/optimize_pass/optimize_pass.h"
#include "compiler/optimize/runtime_linker/runtime_linker.h"
#include "compiler/optimize/target/target.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"

namespace tz {

OptimizePass::OptimizePass(const Target &target, const BuildOptions &options)
    : target_(target), options_(options) {}

Errors OptimizePass::run(llvm::Module &module, bool parallel) const {
  Errors errors;
  if (options_.optimize) {
    // The runtime of the threads, or the one of a single thread (see
    // TZ_PARALLEL in runtime/core/tzrt/tzrt.h).
    RuntimeLinker linker;
    errors = linker.run(module, options_.runtime.path(parallel ? "tzrt_mt.bc" : "tzrt.bc"));
    if (errors) {
      return errors;
    }
  }
  runPasses(module);

  return errors;
}

void OptimizePass::runPasses(llvm::Module &module) const {
  // The analysis managers must be destroyed in this reverse order.
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

  llvm::PassBuilder pb(&target_.machine());
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  llvm::ModulePassManager mpm;
  if (options_.optimize) {
    // First drop what RuntimeLinker left unused, so it is not optimized for
    // nothing.
    mpm.addPass(llvm::GlobalDCEPass());
    mpm.addPass(pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2));
  } else {
    mpm.addPass(pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0));
  }
  mpm.run(module, mam);
}

} // namespace tz
