#include "compiler/instances/instances_pass/instances_pass.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/instances/constructor_calls/constructor_calls.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/program/program/program.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

Errors InstancesPass::run(Program &program) const {
  Checker &checker = *program.checker;
  // The copies of generic functions that calls need, now that every generic
  // body is checked, and the classes that class expressions made, which may
  // need more of each other.
  do {
    checker.lateClasses().checkLateClasses();
    checker.genericInstantiator().checkInstances();
  } while (checker.lateChecked_ < checker.lateClasses_.size());
  checker.constructorCalls().checkConstructorCalls();

  return program.diags->take();
}

} // namespace tz
