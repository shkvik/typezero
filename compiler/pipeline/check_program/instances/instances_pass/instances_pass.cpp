#include "compiler/pipeline/check_program/instances/instances_pass/instances_pass.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/pipeline/check_program/instances/constructor_calls/constructor_calls.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/topics/stmt/var_checker/var_checker.h"

namespace tz {

InstancesPass::InstancesPass(CheckState &owner)
    : state(owner) {}

Errors InstancesPass::run() const {
  do {
    LateClasses(this->state).checkLateClasses();
    GenericInstantiator(this->state).checkInstances();
  } while (this->state.getLateClassStore().hasUncheckedLateClasses());
  ConstructorCalls(this->state).checkConstructorCalls();
  VarChecker(this->state).reportUncheckedResults();

  return this->state.getDiagnostics().take();
}

}
