#pragma once

#include "compiler/semantic/code/iterators/iterable_sources/iterable_sources.h"
#include "compiler/semantic/code/iterators/iterator_signatures/iterator_signatures.h"
#include "compiler/semantic/code/iterators/yield_expressions/yield_expressions.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeIteratorsRegistry {
public:
  explicit CodeIteratorsRegistry(SemanticState &owner);

  void afterSoleIteratorReturn();
  void checkYield(const CheckYield &task);
  void iteratorOf(const IteratorOf &task);
  void finishYield(const FinishYield &task);
  void nextValueOf(const NextValueOf &task);
  void findIterator(const FindIterator &task);
  void userIterator(const UserIterator &task);
  void elementTypeOf(const ElementTypeOf &task);
  void iterableOfSlot(const IterableOfSlot &task);
  void afterYieldValue(const AfterYieldValue &task);
  void buildIterateCall(const BuildIterateCall &task);
  void afterNextMethods(const AfterNextMethods &task);
  void walkIterableParam(const WalkIterableParam &task);
  void afterIterableUser(const AfterIterableUser &task);
  void asyncUserIterator(const AsyncUserIterator &task);
  void finishIterateCall(const FinishIterateCall &task);
  void finishIteratorArgs(const FinishIteratorArgs &task);
  void afterYieldDelegate(const AfterYieldDelegate &task);
  void afterIteratorValue(const AfterIteratorValue &task);
  void resolveIteratorType(const ResolveIteratorType &task);
  void finishIterableParam(const FinishIterableParam &task);
  void walkGeneratorReturn(const WalkGeneratorReturn &task);
  void afterSymbolIterator(const AfterSymbolIterator &task);
  void afterResultIterator(const AfterResultIterator &task);
  void finishIterateSymbol(const FinishIterateSymbol &task);
  void finishIteratorResult(const FinishIteratorResult &task);
  void afterBuiltinIterator(const AfterBuiltinIterator &task);
  void destructuredIteratorSlot(const DestructuredIteratorSlot &task);
  void afterAsyncSymbolIterator(const AfterAsyncSymbolIterator &task);

private:
  IteratorSignatures iteratorSignatures;
  IterableSources iterableSources;
  YieldExpressions yieldExpressions;
};

}
