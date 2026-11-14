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
  void afterAsyncIterableReturn();
  void checkYield(const CheckYield &task);
  void iteratorOf(const IteratorOf &task);
  void finishYield(const FinishYield &task);
  void nextValueOf(const NextValueOf &task);
  void findIterator(const FindIterator &task);
  void userIterator(const UserIterator &task);
  void elementTypeOf(const ElementTypeOf &task);
  void iterableOfSlot(const IterableOfSlot &task);
  void afterYieldValue(const AfterYieldValue &task);
  void afterNextMethods(const AfterNextMethods &task);
  void buildIterateCall(const BuildIterateCall &task);
  void afterIterableUser(const AfterIterableUser &task);
  void asyncUserIterator(const AsyncUserIterator &task);
  void finishIterateCall(const FinishIterateCall &task);
  void walkIterableParam(const WalkIterableParam &task);
  void afterIteratorValue(const AfterIteratorValue &task);
  void afterYieldDelegate(const AfterYieldDelegate &task);
  void finishIteratorArgs(const FinishIteratorArgs &task);
  void afterResultIterator(const AfterResultIterator &task);
  void afterSymbolIterator(const AfterSymbolIterator &task);
  void asyncIterableTypeOf(const AsyncIterableTypeOf &task);
  void finishIterableParam(const FinishIterableParam &task);
  void finishIterateSymbol(const FinishIterateSymbol &task);
  void resolveIteratorType(const ResolveIteratorType &task);
  void walkGeneratorReturn(const WalkGeneratorReturn &task);
  void afterBuiltinIterator(const AfterBuiltinIterator &task);
  void finishIteratorResult(const FinishIteratorResult &task);
  void afterAsyncSymbolIterator(const AfterAsyncSymbolIterator &task);
  void destructuredIteratorSlot(const DestructuredIteratorSlot &task);

private:
  IteratorSignatures iteratorSignatures;
  IterableSources iterableSources;
  YieldExpressions yieldExpressions;
};

}
