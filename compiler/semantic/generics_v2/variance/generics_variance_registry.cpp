#include "compiler/semantic/generics_v2/variance/generics_variance_registry.h"

namespace tz {

GenericsVarianceRegistry::GenericsVarianceRegistry(SemanticState &owner)
    : state(owner) {}

void GenericsVarianceRegistry::joinInstances() {
  auto &tasks = this->state.walk.tasks;
  Expr *expr = tasks.takeChecked();
  auto joined = this->joinedClass(expr->type);
  if (!joined.has_value()) {
    return tasks.pushChecked(expr);
  }
  tasks.pushTask(AfterJoinedInstance{.expr = expr});

  return tasks.pushTask(InstantiateClassType{
      .classDecl = joined->generic,
      .args = std::move(joined->args),
      .loc = expr->loc(),
  });
}

std::optional<GenericsVarianceRegistry::JoinedClass>
GenericsVarianceRegistry::joinedClass(const Type *type) {
  if (!type->isUnion()) {
    return std::nullopt;
  }
  llvm::ArrayRef<const Type *> members = type->members();
  const ClassDecl *first = members.front()->classDecl();
  if (!members.front()->isClass() || first == nullptr) {
    return std::nullopt;
  }
  auto &variance = this->state.generics.variance;
  ClassDecl *generic = variance.sharedGeneric(first, members);
  if (generic == nullptr) {
    return std::nullopt;
  }
  auto instances = variance.instancesOf(members, generic);
  auto args = variance.joinedArgs(generic, instances);
  if (!args.has_value()) {
    return std::nullopt;
  }

  return JoinedClass{.generic = generic, .args = std::move(*args)};
}

void GenericsVarianceRegistry::afterJoinedInstance(const AfterJoinedInstance &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *expr = task.expr;
  const Type *wideType = tasks.takeAnswer();
  tasks.pushChecked(expr);
  if (wideType->isError()) {
    return;
  }
  if (this->state.types.relations.isImplicitlyConvertible(expr->type, wideType)) {
    return tasks.pushTask(Convert{.to = wideType});
  }
}

}
