#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/returns/code_returns_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeReturnsRegistry {
public:
  explicit CodeReturnsRegistry(SemanticState &owner);

  void walkReturn(const WalkReturn &task);
  void afterValueCoerce(const AfterValueCoerce &task);
  void afterReturnValue(const AfterReturnValue &task);
  void afterTestedReturn(const AfterTestedReturn &task);
  void afterPredicateFacts(const AfterPredicateFacts &task);
  void afterInferredReturn(const AfterInferredReturn &task);
  void afterPredicateReturn(const AfterPredicateReturn &task);
  void afterUnlistenedErrors(const AfterUnlistenedErrors &task);
  void afterConstructorValue(const AfterConstructorValue &task);
  void afterAsyncGeneratorReturn(const AfterAsyncGeneratorReturn &task);

private:
  using ResultVoidReturnsEmpty = Result<const Type *, CodeReturnsRegistryVoidReturnError>;
  using ResultPromiseTypeMatch = Result<const Type *, CodeReturnsRegistryPromiseReturnError>;
  using ResultValueNotRequired = Result<const Type *, CodeReturnsRegistryValueRequiredError>;
  using ResultGeneratorReturns = Result<const Type *, CodeReturnsRegistryGeneratorReturnError>;
  using ResultAwaitNotReturned = Result<const ReturnStmt *, CodeReturnsRegistryAwaitReturnedError>;
  using ResultConstructorExits =
      Result<const FuncSymbol *, CodeReturnsRegistryConstructorReturnError>;
  using ResultInsideOfFunction =
      Result<const FuncSymbol *, CodeReturnsRegistryEnclosingFunctionError>;

  void startStrayValue(ReturnStmt *returnStmt);
  void startInferredReturn(ReturnStmt *returnStmt);
  void walkVoidReturn(ReturnStmt *returnStmt, const FuncSymbol *function);
  void startConstructorReturn(ReturnStmt *returnStmt, const FuncSymbol *function);
  void startReturnValue(ReturnStmt *returnStmt, const Type *hint, bool isTested);
  void walkPromiseReturn(ReturnStmt *returnStmt, const FuncSymbol *function, const Type *returned);
  void walkBareReturn(const ReturnStmt *returnStmt, const FuncSymbol *function,
                      const Type *returned);


  bool isShapedReturn(const ReturnStmt *earlier) const;


  const Type *earlierReturnShape() const;
  const Type *declaredReturn(const FuncSymbol *function) const;


  ResultInsideOfFunction checkInsideFunction() const;
  ResultVoidReturnsEmpty checkVoidReturn(const Type *type, const FuncSymbol *function) const;
  ResultValueNotRequired checkValueNotRequired(const Type *returned,
                                               const FuncSymbol *function) const;
  ResultAwaitNotReturned checkAwaitNotReturned(const ReturnStmt *returnStmt,
                                               const FuncSymbol *function) const;
  ResultConstructorExits checkConstructorReturn(const ReturnStmt *returnStmt,
                                                const FuncSymbol *function) const;
  ResultPromiseTypeMatch checkPromiseReturn(const Type *given, const Type *returned,
                                            const FuncSymbol *function) const;
  ResultGeneratorReturns checkGeneratorReturn(const Type *given, const Type *returned,
                                              const FuncSymbol *function) const;

  SemanticState &state;
};

}
