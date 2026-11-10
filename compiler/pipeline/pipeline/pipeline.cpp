#include "compiler/pipeline/pipeline/pipeline.h"
#include "compiler/emit/emit_pass/emit_pass.h"
#include "compiler/emit/ir_writer/ir_writer.h"
#include "compiler/link/licenses_pass/licenses_pass.h"
#include "compiler/link/link_pass/link_pass.h"
#include "compiler/lower/library/lowerer_registry/lowerer_registry.h"
#include "compiler/lower/lower_pass/lower_pass.h"
#include "compiler/memory/memory_pass/memory_pass.h"
#include "compiler/optimize/optimize_pass/optimize_pass.h"
#include "compiler/optimize/target/target.h"
#include "compiler/parallel/parallel_pass/parallel_pass.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/program/program/program.h"
#include "llvm/Support/Path.h"

namespace tz {

ResultBuiltProgram buildProgram(Program &program, const BuildOptions &options) {
  auto target = Target::host(options.optimize);
  if (target.isErr()) {
    return err(target.error());
  }

  ParallelPass parallel;
  ParallelPlan plan = parallel.run(program);

  MemoryPass memory;
  MemoryPlan memoryPlan = memory.run(program);

  llvm::LLVMContext context;
  LowererRegistry lowerers;
  LowerPass lower(context, target.value(), llvm::sys::path::filename(options.input), lowerers,
                  options.debug, options.optimize);
  auto lowered = lower.run(program, plan, memoryPlan);
  if (lowered.isErr()) {
    return err(lowered.error());
  }
  llvm::Module &module = *lowered.value().module;

  OptimizePass optimize(target.value(), options);
  std::vector<Diagnostic> optimizeErrors = optimize.run(module, lowered.value().parallel);
  if (!optimizeErrors.empty()) {
    return err(optimizeErrors);
  }

  if (options.emitLLVM) {
    IrWriter writer;
    std::vector<Diagnostic> writeErrors = writer.run(module, options.output);
    if (!writeErrors.empty()) {
      return err(writeErrors);
    }
    return ok(BuiltProgram{});
  }

  LicensesPass licenses;
  CryptoUse crypto = licenses.run(module);

  EmitPass emit(target.value());
  auto object = emit.run(module);
  if (object.isErr()) {
    return err(object.error());
  }

  LinkPass link(target.value(), options);
  auto linked = link.run(object.value(), crypto, lowered.value().parallel);
  if (linked.isErr()) {
    return err(linked.error());
  }

  return ok(BuiltProgram{
      .warnings = linked.value(),
  });
}

}
