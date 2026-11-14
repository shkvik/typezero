#include "compiler/semantic/code/literals/code_literals_registry.h"

namespace tz {

CodeLiteralsRegistry::CodeLiteralsRegistry(SemanticState &owner)
    : literalRules(owner),
      arrayLiterals(owner),
      scalarLiterals(owner),
      spreadLiterals(owner, this->literalRules),
      methodLiterals(owner),
      objectLiteralKeys(owner),
      objectLiteralProperties(owner, this->literalRules) {}

void CodeLiteralsRegistry::checkIntLiteral(const CheckIntLiteral &task) {
  return this->scalarLiterals.checkIntLiteral(task);
}

void CodeLiteralsRegistry::checkTemplate(const CheckTemplate &task) {
  return this->scalarLiterals.checkTemplate(task);
}

void CodeLiteralsRegistry::nextTemplatePart(const NextTemplatePart &task) {
  return this->scalarLiterals.nextTemplatePart(task);
}

void CodeLiteralsRegistry::afterTemplatePart(const AfterTemplatePart &task) {
  return this->scalarLiterals.afterTemplatePart(task);
}

void CodeLiteralsRegistry::afterTemplateType(const AfterTemplateType &task) {
  return this->scalarLiterals.afterTemplateType(task);
}

void CodeLiteralsRegistry::checkObjectLiteral(const CheckObjectLiteral &task) {
  return this->objectLiteralKeys.checkObjectLiteral(task);
}

void CodeLiteralsRegistry::afterComputedKey(const AfterComputedKey &task) {
  return this->objectLiteralKeys.afterComputedKey(task);
}

void CodeLiteralsRegistry::afterUnionNeeds(const AfterUnionNeeds &task) {
  return this->objectLiteralKeys.afterUnionNeeds(task);
}

void CodeLiteralsRegistry::nextInferredKey(const NextInferredKey &task) {
  return this->objectLiteralProperties.nextInferredKey(task);
}

void CodeLiteralsRegistry::afterInferredKey(const AfterInferredKey &task) {
  return this->objectLiteralProperties.afterInferredKey(task);
}

void CodeLiteralsRegistry::nextKeyedProperty(const NextKeyedProperty &task) {
  return this->objectLiteralProperties.nextKeyedProperty(task);
}

void CodeLiteralsRegistry::nextNamedProperty(const NextNamedProperty &task) {
  return this->objectLiteralProperties.nextNamedProperty(task);
}

void CodeLiteralsRegistry::afterUntargetedField(const AfterUntargetedField &task) {
  return this->objectLiteralProperties.afterUntargetedField(task);
}

void CodeLiteralsRegistry::nextRecordProperty(const NextRecordProperty &task) {
  return this->spreadLiterals.nextRecordProperty(task);
}

void CodeLiteralsRegistry::afterRecordSpread(const AfterRecordSpread &task) {
  return this->spreadLiterals.afterRecordSpread(task);
}

void CodeLiteralsRegistry::nextSpreadProperty(const NextSpreadProperty &task) {
  return this->spreadLiterals.nextSpreadProperty(task);
}

void CodeLiteralsRegistry::afterNamedProp(const AfterNamedProp &task) {
  return this->spreadLiterals.afterNamedProp(task);
}

void CodeLiteralsRegistry::afterSpreadProp(const AfterSpreadProp &task) {
  return this->spreadLiterals.afterSpreadProp(task);
}

void CodeLiteralsRegistry::nextLiteralMember(const NextLiteralMember &task) {
  return this->methodLiterals.nextLiteralMember(task);
}

void CodeLiteralsRegistry::afterLiteralField(const AfterLiteralField &task) {
  return this->methodLiterals.afterLiteralField(task);
}

void CodeLiteralsRegistry::walkLiteralMethods(const WalkLiteralMethods &task) {
  return this->methodLiterals.walkLiteralMethods(task);
}

void CodeLiteralsRegistry::walkLiteralMethod(const WalkLiteralMethod &task) {
  return this->methodLiterals.walkLiteralMethod(task);
}

void CodeLiteralsRegistry::afterLiteralMethodReturn(const AfterLiteralMethodReturn &task) {
  return this->methodLiterals.afterLiteralMethodReturn(task);
}

void CodeLiteralsRegistry::afterLiteralMethods(const AfterLiteralMethods &task) {
  return this->methodLiterals.afterLiteralMethods(task);
}

void CodeLiteralsRegistry::matchLiteralTarget(const MatchLiteralTarget &task) {
  return this->methodLiterals.matchLiteralTarget(task);
}

void CodeLiteralsRegistry::checkArrayLiteral(const CheckArrayLiteral &task) {
  return this->arrayLiterals.checkArrayLiteral(task);
}

void CodeLiteralsRegistry::finishPairLiteral(const FinishPairLiteral &task) {
  return this->arrayLiterals.finishPairLiteral(task);
}

void CodeLiteralsRegistry::nextArrayElement(const NextArrayElement &task) {
  return this->arrayLiterals.nextArrayElement(task);
}

void CodeLiteralsRegistry::afterLiteralElement(const AfterLiteralElement &task) {
  return this->arrayLiterals.afterLiteralElement(task);
}

void CodeLiteralsRegistry::afterSpreadElement(const AfterSpreadElement &task) {
  return this->arrayLiterals.afterSpreadElement(task);
}

void CodeLiteralsRegistry::afterSpreadMatch(const AfterSpreadMatch &task) {
  return this->arrayLiterals.afterSpreadMatch(task);
}

}
