#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/iterators/code_iterators_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class YieldExpressions {
public:
  explicit YieldExpressions(SemanticState &owner);

  void checkYield(const CheckYield &task);
  void finishYield(const FinishYield &task);
  void afterYieldValue(const AfterYieldValue &task);
  void afterYieldDelegate(const AfterYieldDelegate &task);

private:
  using ResultNothingSentAsync = Result<const Type *, CodeIteratorsRegistryAsyncSentError>;
  using ResultDelegateSyncFits = Result<const Type *, CodeIteratorsRegistryDelegateSyncError>;
  using ResultPromiseYieldFits = Result<const Type *, CodeIteratorsRegistryPromiseYieldError>;
  using ResultSentTypeDeclared = Result<const Type *, CodeIteratorsRegistrySentDeclaredError>;
  using ResultYieldGivesAValue = Result<const Type *, CodeIteratorsRegistryValueYieldedError>;
  using ResultDelegateAsyncFit = Result<const Type *, CodeIteratorsRegistryDelegateAsyncError>;

  void startStrayYield(YieldExpr *yield);
  void startDelegate(YieldExpr *yield, const YieldShape &shape);
  void startYieldValue(YieldExpr *yield, const YieldShape &shape);
  void walkSyncDelegate(YieldExpr *yield, const YieldShape &shape);
  void walkAsyncDelegate(YieldExpr *yield, const YieldShape &shape);
  void walkAsyncYieldValue(YieldExpr *yield, const YieldShape &shape);
  void walkEmptyYield(const YieldExpr *yield, const YieldShape &shape);


  bool isIteratorKind(const Type *type, bool isAsync) const;


  YieldShape yieldShape(const YieldExpr *yield, FuncSymbol *function);
  const Type *yieldResultType(const YieldExpr *yield, const YieldShape &shape) const;
  const Type *delegateResultType(const YieldExpr *yield, const YieldShape &shape) const;
  const Type *asyncSentType(const YieldExpr *yield, const Type *sent, const Type *yielded,
                            bool isLoose);


  ResultYieldGivesAValue checkValueYielded(const YieldShape &shape) const;
  ResultSentTypeDeclared checkSentDeclared(const YieldExpr *yield, const YieldShape &shape) const;
  ResultNothingSentAsync checkAsyncSent(const Type *sent, const Type *yielded, bool isLoose) const;
  ResultPromiseYieldFits checkPromiseYield(const Type *type, const Type *given,
                                           const YieldShape &shape) const;
  ResultDelegateSyncFits checkDelegateSync(const Type *type, const Type *element,
                                           const YieldShape &shape) const;
  ResultDelegateAsyncFit checkDelegateAsync(const Type *type, const Type *element,
                                            const YieldShape &shape) const;


  std::string shownYieldedName(const Type *yielded) const;

  SemanticState &state;
};

}
