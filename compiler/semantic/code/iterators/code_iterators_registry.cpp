#include "compiler/semantic/code/iterators/code_iterators_registry.h"

namespace tz {

CodeIteratorsRegistry::CodeIteratorsRegistry(SemanticState &owner)
    : iteratorSignatures(owner),
      iterableSources(owner),
      yieldExpressions(owner) {}

void CodeIteratorsRegistry::resolveIteratorType(const ResolveIteratorType &task) {
  return this->iteratorSignatures.resolveIteratorType(task);
}

void CodeIteratorsRegistry::finishIteratorArgs(const FinishIteratorArgs &task) {
  return this->iteratorSignatures.finishIteratorArgs(task);
}

void CodeIteratorsRegistry::finishIteratorResult(const FinishIteratorResult &task) {
  return this->iteratorSignatures.finishIteratorResult(task);
}

void CodeIteratorsRegistry::walkIterableParam(const WalkIterableParam &task) {
  return this->iteratorSignatures.walkIterableParam(task);
}

void CodeIteratorsRegistry::finishIterableParam(const FinishIterableParam &task) {
  return this->iteratorSignatures.finishIterableParam(task);
}

void CodeIteratorsRegistry::walkGeneratorReturn(const WalkGeneratorReturn &task) {
  return this->iteratorSignatures.walkGeneratorReturn(task);
}

void CodeIteratorsRegistry::checkYield(const CheckYield &task) {
  return this->yieldExpressions.checkYield(task);
}

void CodeIteratorsRegistry::afterYieldValue(const AfterYieldValue &task) {
  return this->yieldExpressions.afterYieldValue(task);
}

void CodeIteratorsRegistry::afterYieldDelegate(const AfterYieldDelegate &task) {
  return this->yieldExpressions.afterYieldDelegate(task);
}

void CodeIteratorsRegistry::finishYield(const FinishYield &task) {
  return this->yieldExpressions.finishYield(task);
}

void CodeIteratorsRegistry::destructuredIteratorSlot(const DestructuredIteratorSlot &task) {
  return this->iterableSources.destructuredIteratorSlot(task);
}

void CodeIteratorsRegistry::iterableOfSlot(const IterableOfSlot &task) {
  return this->iterableSources.iterableOfSlot(task);
}

void CodeIteratorsRegistry::afterIterableUser(const AfterIterableUser &task) {
  return this->iterableSources.afterIterableUser(task);
}

void CodeIteratorsRegistry::findIterator(const FindIterator &task) {
  return this->iterableSources.findIterator(task);
}

void CodeIteratorsRegistry::afterBuiltinIterator(const AfterBuiltinIterator &task) {
  return this->iterableSources.afterBuiltinIterator(task);
}

void CodeIteratorsRegistry::userIterator(const UserIterator &task) {
  return this->iterableSources.userIterator(task);
}

void CodeIteratorsRegistry::afterSymbolIterator(const AfterSymbolIterator &task) {
  return this->iterableSources.afterSymbolIterator(task);
}

void CodeIteratorsRegistry::afterResultIterator(const AfterResultIterator &task) {
  return this->iterableSources.afterResultIterator(task);
}

void CodeIteratorsRegistry::asyncUserIterator(const AsyncUserIterator &task) {
  return this->iterableSources.asyncUserIterator(task);
}

void CodeIteratorsRegistry::afterAsyncSymbolIterator(const AfterAsyncSymbolIterator &task) {
  return this->iterableSources.afterAsyncSymbolIterator(task);
}

void CodeIteratorsRegistry::iteratorOf(const IteratorOf &task) {
  return this->iterableSources.iteratorOf(task);
}

void CodeIteratorsRegistry::afterIteratorValue(const AfterIteratorValue &task) {
  return this->iterableSources.afterIteratorValue(task);
}

void CodeIteratorsRegistry::finishIterateSymbol(const FinishIterateSymbol &task) {
  return this->iterableSources.finishIterateSymbol(task);
}

void CodeIteratorsRegistry::buildIterateCall(const BuildIterateCall &task) {
  return this->iterableSources.buildIterateCall(task);
}

void CodeIteratorsRegistry::finishIterateCall(const FinishIterateCall &task) {
  return this->iterableSources.finishIterateCall(task);
}

void CodeIteratorsRegistry::nextValueOf(const NextValueOf &task) {
  return this->iteratorSignatures.nextValueOf(task);
}

void CodeIteratorsRegistry::afterNextMethods(const AfterNextMethods &task) {
  return this->iteratorSignatures.afterNextMethods(task);
}

void CodeIteratorsRegistry::elementTypeOf(const ElementTypeOf &task) {
  return this->iteratorSignatures.elementTypeOf(task);
}

void CodeIteratorsRegistry::afterSoleIteratorReturn() {
  return this->iteratorSignatures.afterSoleIteratorReturn();
}

void CodeIteratorsRegistry::asyncIterableTypeOf(const AsyncIterableTypeOf &task) {
  return this->iteratorSignatures.asyncIterableTypeOf(task);
}

void CodeIteratorsRegistry::afterAsyncIterableReturn() {
  return this->iteratorSignatures.afterAsyncIterableReturn();
}

}
