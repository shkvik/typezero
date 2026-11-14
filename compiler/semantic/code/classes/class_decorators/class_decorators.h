#pragma once

#include "compiler/semantic/code/classes/class_decorators/decorated_classes/decorated_classes.h"
#include "compiler/semantic/code/classes/class_decorators/decorator_applying/decorator_applying.h"
#include "compiler/semantic/code/classes/class_decorators/decorator_hoisting/decorator_hoisting.h"
#include "compiler/semantic/code/classes/class_decorators/decorator_rules/decorator_rules.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ClassDecorators {
public:
  ClassDecorators(SemanticState &owner, ClassFacts &sharedFacts);

  void leaveClassContext();
  void dropDiagnosticList();
  void decorate(const Decorate &task);
  void relinkClass(const RelinkClass &task);
  void decorateClass(const DecorateClass &task);
  void makeMethodDescriptor(const MakeMethodDescriptor &task);
  void nextAppliedDecorator(const NextAppliedDecorator &task);
  void nextHoistedDecorator(const NextHoistedDecorator &task);
  void afterDecoratorApplied(const AfterDecoratorApplied &task);
  void afterDescriptorDeclared(const AfterDescriptorDeclared &task);
  void declareHoistedDecorator(const DeclareHoistedDecorator &task);
  void afterFactoryMethodReturn(const AfterFactoryMethodReturn &task);
  void afterDescriptorMethodReturn(const AfterDescriptorMethodReturn &task);

private:
  DecoratorRules decoratorRules;
  DecoratedClasses decoratedClasses;
  DecoratorHoisting decoratorHoisting;
  DecoratorApplying decoratorApplying;
};

}
