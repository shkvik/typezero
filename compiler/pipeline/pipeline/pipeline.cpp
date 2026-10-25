#include "compiler/pipeline/pipeline/pipeline.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/check/check_pass/check_pass.h"
#include "compiler/check/event_rounds/event_rounds.h"
#include "compiler/declare/declare_pass/declare_pass.h"
#include "compiler/emit/emit_pass/emit_pass.h"
#include "compiler/emit/ir_writer/ir_writer.h"
#include "compiler/infer/infer_pass/infer_pass.h"
#include "compiler/instances/instances_pass/instances_pass.h"
#include "compiler/link/licenses_pass/licenses_pass.h"
#include "compiler/link/link_pass/link_pass.h"
#include "compiler/load/load_pass/load_pass.h"
#include "compiler/lower/lower_pass/lower_pass.h"
#include "compiler/optimize/optimize_pass/optimize_pass.h"
#include "compiler/optimize/target/target.h"
#include "compiler/overflow/overflow_pass/overflow_pass.h"
#include "compiler/parallel/parallel_pass/parallel_pass.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/program/program/program.h"
#include "compiler/whole_program/whole_program_pass/whole_program_pass.h"
#include "llvm/Support/Path.h"

namespace tz {

Errors checkProgram(Program &program, const BuildOptions &options) {
  const FeatureRegistry &features = FeatureRegistry::standard();
  EventRounds rounds;
  for (;;) {
    LoadPass load(options.input, options.packages);
    Errors errors = load.run(program);
    if (errors) {
      return errors;
    }

    DeclarePass declare(rounds.inference(), features);
    errors.add(declare.run(program));

    InferPass infer;
    errors.add(infer.run(program));

    CheckPass check;
    errors.add(check.run(program));

    InstancesPass instances;
    errors.add(instances.run(program));

    WholeProgramPass whole;
    errors.add(whole.run(program));
    if (rounds.again()) {
      continue;
    }

    OverflowPass overflow;
    errors.add(overflow.run(program));

    return errors;
  }
}

Errors buildProgram(Program &program, const BuildOptions &options) {
  Result<Target, Errors> target = Target::host(options.optimize);
  if (target.isErr()) {
    return target.error();
  }

  ParallelPass parallel;
  ParallelPlan plan = parallel.run(program);

  llvm::LLVMContext context;
  LowerPass lower(context, target.value(), llvm::sys::path::filename(options.input),
                  FeatureRegistry::standard());
  Result<Lowered, Errors> lowered = lower.run(program, plan);
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

} // namespace tz
