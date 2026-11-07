#include "compiler/pipeline/pipeline/pipeline.h"
#include "compiler/emit/emit_pass/emit_pass.h"
#include "compiler/emit/ir_writer/ir_writer.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/link/licenses_pass/licenses_pass.h"
#include "compiler/link/link_pass/link_pass.h"
#include "compiler/lower/lower_pass/lower_pass.h"
#include "compiler/memory/memory_pass/memory_pass.h"
#include "compiler/optimize/optimize_pass/optimize_pass.h"
#include "compiler/optimize/target/target.h"
#include "compiler/parallel/parallel_pass/parallel_pass.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/program/program/program.h"
#include "llvm/Support/Path.h"

namespace tz {

Errors buildProgram(Program &program, const BuildOptions &options) {
  Result<Target, Errors> target = Target::host(options.optimize);
  if (target.isErr()) {
    return target.error();
  }

  ParallelPass parallel;
  ParallelPlan plan = parallel.run(program);

  MemoryPass memory;
  MemoryPlan memoryPlan = memory.run(program);

  llvm::LLVMContext context;
  FeatureRegistry features;
  LowerPass lower(context, target.value(), llvm::sys::path::filename(options.input), features,
                  options.debug, options.optimize);
  Result<Lowered, Errors> lowered = lower.run(program, plan, memoryPlan);
  if (lowered.isErr()) {
    return lowered.error();
  }
  llvm::Module &module = *lowered.value().module;

  OptimizePass optimize(target.value(), options);
  Errors optimizeErrors = optimize.run(module, lowered.value().parallel);
  if (optimizeErrors) {
    return optimizeErrors;
  }

  if (options.emitLLVM) {
    IrWriter writer;
    return writer.run(module, options.output);
  }

  LicensesPass licenses;
  CryptoUse crypto = licenses.run(module);

  EmitPass emit(target.value());
  Result<ObjectFile, Errors> object = emit.run(module);
  if (object.isErr()) {
    return object.error();
  }

  LinkPass link(target.value(), options);

  return link.run(object.value(), crypto, lowered.value().parallel);
}

}
