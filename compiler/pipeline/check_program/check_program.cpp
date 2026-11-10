#include "compiler/pipeline/check_program/check_program.h"
#include "compiler/load/load_pass/load_pass.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/check_program/check_pass/check_pass.h"
#include "compiler/pipeline/check_program/declare_pass/declare_pass.h"
#include "compiler/pipeline/check_program/infer_pass/infer_pass.h"
#include "compiler/pipeline/check_program/instances_pass/instances_pass.h"
#include "compiler/pipeline/check_program/interprocedural_pass/interprocedural_pass.h"
#include "compiler/pipeline/check_program/overflow_pass/overflow_pass.h"
#include "compiler/pipeline/pass_times/pass_times.h"
#include "compiler/program/program/program.h"
#include "compiler/semantic/semantic_external.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

Errors checkProgram(Program &program, const BuildOptions &options, PassTimes *times) {
  PassTimes unused;
  PassTimes &clock = times ? *times : unused;
  clock.start();
  LoadPass load(options.input, options.packages, options.overlay);
  Errors errors = load.run(program);
  clock.stop("load");
  if (errors) {
    return errors;
  }

  SemanticExternalFacade engine(*program.asts.back(), *program.diags, program.file);
  DeclarePass declare(engine, program);
  errors.add(declare.run());
  clock.stop("declare");

  InferPass infer(engine);
  errors.add(infer.run());
  clock.stop("infer");

  CheckPass check(engine);
  CheckPass::ResultChecked checked = check.run();
  if (checked.isErr()) {
    errors.add(checked.error());
  } else {
    PassDone &done = checked.value();
    switch (done.kind()) {
      case PassDoneKind::NO_WARNINGS:
        break;
      case PassDoneKind::WITH_WARNINGS:
        errors.add(done.as<WithWarnings>()->warnings);
        break;
    }
  }
  clock.stop("check");

  InstancesPass instances(engine);
  errors.add(instances.run());
  clock.stop("instances");

  InterproceduralPass interprocedural(engine, program);
  interprocedural.checkLoadOrder();
  engine.finishProgram();
  engine.collectInventory();
  program.file.inventory = engine.takeInventory();
  llvm::append_range(program.file.items, program.file.inventory.lateClasses);
  errors.add(interprocedural.checkValueFlows());
  clock.stop("whole program");
  if (!options.overflow) {
    return errors;
  }

  OverflowPass overflow;
  errors.add(overflow.run(program));
  clock.stop("overflow");

  return errors;
}

}
