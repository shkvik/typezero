#include "compiler/semantic/generics_v2/classes/instance_filling/instance_filling.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

InstanceFilling::InstanceFilling(SemanticState &owner)
    : state(owner) {}

void InstanceFilling::fillUnfilledInstances() {
  auto unfilled = this->state.generics.copies.takeUnfilledClasses();
  for (ClassDecl *instance : llvm::reverse(unfilled)) {
    this->state.walk.tasks.pushTask(FillInstance{.instance = instance});
  }
}

void InstanceFilling::fillInstance(const FillInstance &task) {
  ClassDecl *instance = task.instance;
  auto &tasks = this->state.walk.tasks;
  this->state.generics.copies.enterFilling(instance);
  tasks.pushTask(LeaveFilling{});
  tasks.pushTask(WalkInstanceVariants{.instance = instance});
  tasks.pushTask(DeclareInstanceMembers{.instance = instance});
  tasks.pushTask(NextFilledField{.instance = instance, .index = 0});
  tasks.pushTask(AddStreamBases{.instance = instance, .loc = instance->loc()});

  return tasks.pushTask(LinkInstanceBases{.instance = instance});
}

void InstanceFilling::leaveFilling() {
  this->state.generics.copies.leaveFilling();
}

void InstanceFilling::linkInstanceBases(const LinkInstanceBases &task) {
  ClassDecl *instance = task.instance;
  auto &tasks = this->state.walk.tasks;
  const ClassDecl *genericParent = instance->generic()->parent;
  if (genericParent == nullptr) {
    return this->nextInstanceInterface(NextInstanceInterface{.instance = instance, .index = 0});
  }
  tasks.pushTask(AfterInstanceParent{.instance = instance});

  return tasks.pushTask(InstanceType{.instance = instance, .type = genericParent->type});
}

void InstanceFilling::afterInstanceParent(const AfterInstanceParent &task) {
  ClassDecl *instance = task.instance;
  auto &tasks = this->state.walk.tasks;
  const Type *parentType = tasks.takeAnswer();
  tasks.pushTask(NextInstanceInterface{.instance = instance, .index = 0});
  auto *parent = const_cast<ClassDecl *>(parentType != nullptr ? parentType->classDecl() : nullptr);
  if (parent == nullptr) {
    return;
  }
  instance->parent = parent;
  parent->children.push_back(instance);

  return this->addInstanceBase(instance, parent->type);
}

void InstanceFilling::nextInstanceInterface(const NextInstanceInterface &task) {
  ClassDecl *instance = task.instance;
  const auto &implemented = instance->generic()->implemented;
  if (task.index == implemented.size()) {
    return;
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInstanceInterface{.instance = instance, .index = task.index});

  return tasks.pushTask(InstanceType{.instance = instance, .type = implemented[task.index]});
}

void InstanceFilling::afterInstanceInterface(const AfterInstanceInterface &task) {
  ClassDecl *instance = task.instance;
  auto &tasks = this->state.walk.tasks;
  const Type *type = tasks.takeAnswer();
  tasks.pushTask(NextInstanceInterface{.instance = instance, .index = task.index + 1});
  if (type->isError() || llvm::is_contained(instance->implemented, type)) {
    return;
  }
  instance->implemented.push_back(type);

  return this->addInstanceBase(instance, type);
}

void InstanceFilling::addInstanceBase(ClassDecl *instance, const Type *base) {
  if (instance->isEmitted()) {
    this->addInventorySubtype(instance->type);
  }

  return this->state.walk.tasks.pushTask(AddBase{.type = instance->type, .base = base});
}

void InstanceFilling::addInventorySubtype(const Type *type) {
  if (!llvm::is_contained(this->state.inventory.subtypes, type)) {
    this->state.inventory.subtypes.push_back(type);
  }
}

void InstanceFilling::nextFilledField(const NextFilledField &task) {
  ClassDecl *instance = task.instance;
  ClassDecl *generic = instance->generic();
  for (size_t index = task.index; index < instance->fields.size() && index < generic->fields.size();
       ++index) {
    ClassField &field = instance->fields[index];
    const ClassField &genericField = generic->fields[index];
    field.presence = genericField.presence;
    field.redeclared = genericField.redeclared;
    if (!field.isStatic()) {
      return this->startFilledField(instance, index);
    }
  }
}

void InstanceFilling::startFilledField(ClassDecl *instance, size_t index) {
  const ClassField &genericField = instance->generic()->fields[index];
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterFilledField{.instance = instance, .index = index});
  if (genericField.method() == nullptr) {
    return tasks.pushTask(InstanceValueType{.instance = instance, .type = genericField.resolved});
  }

  return tasks.pushTask(InstanceType{.instance = instance, .type = genericField.resolved});
}

void InstanceFilling::afterFilledField(const AfterFilledField &task) {
  ClassDecl *instance = task.instance;
  ClassField &field = instance->fields[task.index];
  const FunctionDecl *genericMethod = instance->generic()->fields[task.index].method();
  field.resolved = this->state.walk.tasks.takeAnswer();
  if (genericMethod != nullptr) {
    FunctionDecl *functionDecl = cast<FunctionExpr>(field.init)->fn;
    functionDecl->name = genericMethod->name;
    functionDecl->role = ClassMember{
        .cls = instance,
        .access = genericMethod->access(),
        .isOverride = genericMethod->isOverride(),
    };
    functionDecl->place = FunctionPlace::DECLARED;
    field.form = ArrowMethodField{.method = functionDecl};
  }

  return this->nextFilledField(NextFilledField{.instance = instance, .index = task.index + 1});
}

}
