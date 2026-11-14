#include "compiler/semantic/generics_v2/constraints/generics_constraints_registry.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"

namespace tz {

GenericsConstraintsRegistry::GenericsConstraintsRegistry(SemanticState &owner)
    : state(owner) {}

void GenericsConstraintsRegistry::checkConstraint(const CheckConstraint &task) {
  auto &tasks = this->state.walk.tasks;
  if (task.arg->isError()) {
    return tasks.pushVerdict(true);
  }
  const Type *written = task.param->constraint();
  bool isDependent = written != nullptr && !task.params.empty() &&
                     GenericCheckHelpers::mentionsAnyOf(written, task.params);
  if (!isDependent) {
    auto check = ConstraintCheck{.arg = task.arg, .param = task.param, .own = written, .known = {}};

    return this->evaluateConstraint(EvaluateConstraint{.check = std::move(check)});
  }
  tasks.pushTask(AfterConstraintSubstitution{.arg = task.arg, .param = task.param});

  return tasks.pushTask(SubstituteType{.type = written, .params = task.params, .args = task.args});
}

void GenericsConstraintsRegistry::afterConstraintSubstitution(
    const AfterConstraintSubstitution &task) {
  const Type *own = this->state.walk.tasks.takeAnswer();
  auto check = ConstraintCheck{.arg = task.arg, .param = task.param, .own = own, .known = {}};

  return this->evaluateConstraint(EvaluateConstraint{.check = std::move(check)});
}

void GenericsConstraintsRegistry::evaluateConstraint(const EvaluateConstraint &task) {
  auto &tasks = this->state.walk.tasks;
  const auto &constraints = this->state.generics.constraints;
  FuncSymbol *missing = constraints.findUnknownMethod(task.check);
  if (missing == nullptr) {
    bool isSatisfied = constraints.isSatisfying(task.check);

    return tasks.pushVerdict(isSatisfied);
  }
  tasks.pushTask(AfterConstraintMethod{.check = task.check, .function = missing});

  return tasks.pushTask(FunctionTypeOf{.function = missing});
}

void GenericsConstraintsRegistry::afterConstraintMethod(const AfterConstraintMethod &task) {
  ConstraintCheck check = task.check;
  const Type *type = this->state.walk.tasks.takeAnswer();
  check.known.push_back({task.function, type});

  return this->evaluateConstraint(EvaluateConstraint{.check = std::move(check)});
}

}
