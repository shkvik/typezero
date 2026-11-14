#include "compiler/semantic/code/classes/class_decorators/class_decorators.h"

namespace tz {

ClassDecorators::ClassDecorators(SemanticState &owner, ClassFacts &sharedFacts)
    : decoratorRules(owner),
      decoratedClasses(owner, this->decoratorRules),
      decoratorHoisting(owner, this->decoratorRules),
      decoratorApplying(owner, sharedFacts, this->decoratorRules) {}

void ClassDecorators::decorateClass(const DecorateClass &task) {
  return this->decoratedClasses.decorateClass(task);
}

void ClassDecorators::leaveClassContext() {
  return this->decoratedClasses.leaveClassContext();
}

void ClassDecorators::makeMethodDescriptor(const MakeMethodDescriptor &task) {
  return this->decoratorHoisting.makeMethodDescriptor(task);
}

void ClassDecorators::decorate(const Decorate &task) {
  return this->decoratorHoisting.decorate(task);
}

void ClassDecorators::nextHoistedDecorator(const NextHoistedDecorator &task) {
  return this->decoratorHoisting.nextHoistedDecorator(task);
}

void ClassDecorators::afterDescriptorMethodReturn(const AfterDescriptorMethodReturn &task) {
  return this->decoratorHoisting.afterDescriptorMethodReturn(task);
}

void ClassDecorators::afterFactoryMethodReturn(const AfterFactoryMethodReturn &task) {
  return this->decoratorHoisting.afterFactoryMethodReturn(task);
}

void ClassDecorators::declareHoistedDecorator(const DeclareHoistedDecorator &task) {
  return this->decoratorHoisting.declareHoistedDecorator(task);
}

void ClassDecorators::afterDescriptorDeclared(const AfterDescriptorDeclared &task) {
  return this->decoratorHoisting.afterDescriptorDeclared(task);
}

void ClassDecorators::nextAppliedDecorator(const NextAppliedDecorator &task) {
  return this->decoratorApplying.nextAppliedDecorator(task);
}

void ClassDecorators::afterDecoratorApplied(const AfterDecoratorApplied &task) {
  return this->decoratorApplying.afterDecoratorApplied(task);
}

void ClassDecorators::relinkClass(const RelinkClass &task) {
  return this->decoratorApplying.relinkClass(task);
}

void ClassDecorators::dropDiagnosticList() {
  return this->decoratorApplying.dropDiagnosticList();
}

}
