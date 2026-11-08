#include "compiler/pipeline/check_program/check_program.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/load/load_pass/load_pass.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/check_program/check/check_pass/check_pass.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/declare/declare_pass/declare_pass.h"
#include "compiler/pipeline/check_program/event_rounds/event_rounds.h"
#include "compiler/pipeline/check_program/infer/infer_pass/infer_pass.h"
#include "compiler/pipeline/check_program/instances/instances_pass/instances_pass.h"
#include "compiler/pipeline/check_program/overflow/overflow_pass/overflow_pass.h"
#include "compiler/pipeline/check_program/whole_program/whole_program_pass/whole_program_pass.h"
#include "compiler/pipeline/pass_times/pass_times.h"
#include "compiler/program/program/program.h"

namespace tz {

Errors checkProgram(Program &program, const BuildOptions &options, PassTimes *times) {
  FeatureRegistry features;
  EventRounds rounds;
  PassTimes unused;
  PassTimes &clock = times ? *times : unused;
  for (;;) {
    clock.round();
    clock.start();
    LoadPass load(options.input, options.packages, options.overlay);
    Errors errors = load.run(program);
    clock.stop("load");
    if (errors) {
      return errors;
    }

    CheckState state(*program.asts.back(), *program.diags, program.file, features,
                     rounds.inference());
    DeclarePass declare(state, program.file);
    errors.add(declare.run());
    clock.stop("declare");

    InferPass infer(state);
    errors.add(infer.run());
    clock.stop("infer");

    CheckPass check(state, program.file);
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

    InstancesPass instances(state);
    errors.add(instances.run());
    clock.stop("instances");

    WholeProgramPass whole(state, program);
    errors.add(whole.run());
    clock.stop("whole program");
    if (rounds.again()) {
      continue;
    }
    if (!options.overflow) {
      return errors;
    }

    OverflowPass overflow;
    errors.add(overflow.run(program));
    clock.stop("overflow");

    return errors;
  }
}

}
