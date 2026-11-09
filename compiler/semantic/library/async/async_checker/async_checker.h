#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/async/async_checker/async_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class LoopBounds;
class DictChecker;
class ExprChecker;
class FlowChecker;
class LoopChecker;
class LoopScanner;
class MatchChecker;
class TypeResolver;
class TypeRelations;
class GenericChecker;
class GenericWidening;
class IterableChecker;
class IteratorMethods;
class GeneratorChecker;

class AsyncChecker {
public:
  using ResultPromiseTypeRef = Result<const Type *, AsyncCheckerResolvePromiseTypeError>;
  using ResultPromiseHandled = Result<const Expr *, AsyncCheckerCheckDiscardedPromiseError>;

  AsyncChecker(SemanticState &owner, SemanticParts &all);

  void checkForAwait(ForOfStmt *stmt);
  static const Type *awaitedType(const Type *type);
  Expr *checkAwait(AwaitExpr *await, const Type *expected);
  ResultPromiseTypeRef resolvePromiseType(const TypeRef &ref);
  const Type *asyncReturnType(const FunctionDecl *fn, const Type *declared);
  void finishAsyncReturns(FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns);
  ResultPromiseHandled checkDiscardedPromise(const Expr *expr, bool callback, llvm::SMLoc at);
  const Type *inferAsyncType(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                             bool fallsOff, bool report);

private:
  using ResultAwaitedOneType = Result<const Type *, AsyncCheckerAwaitableError>;
  using ResultReturnsOneType = Result<const Type *, AsyncCheckerReturnsJoinError>;
  using ResultBindingPattern = Result<const Type *, AsyncCheckerBindingShapeError>;
  using ResultPromiseMatches = Result<const Type *, AsyncCheckerPromiseMatchesError>;
  using ResultAwaitedElement = Result<const Type *, AsyncCheckerAwaitedElementError>;
  using ResultReturnsPromise = Result<const Type *, AsyncCheckerPromiseDeclaredError>;
  using ResultInAsyncContext = Result<const FuncSymbol *, AsyncCheckerAsyncContextError>;
  using ResultBindingUntyped = Result<const VarDeclarator *, AsyncCheckerBindingTypeError>;

  static const Type *awaited(const Type *type);
  static std::string statementText(llvm::SMLoc at);
  ResultInAsyncContext asyncContext(bool loop) const;
  static ResultAwaitedOneType awaitable(const Type *type);
  ResultAwaitedElement awaitedElement(const Type *type) const;
  static ResultBindingUntyped bindingType(const VarDeclarator &binding);
  static ResultReturnsPromise promiseDeclared(const FunctionDecl *fn, const Type *declared);
  static ResultBindingPattern bindingShape(const VarDeclarator &binding, const Type *element);
  static ResultReturnsOneType returnsJoin(const Type *type, const Type *t, const FuncSymbol *fn);
  static ResultPromiseMatches promiseMatches(const Type *given, const Type *value,
                                             const FuncSymbol *fn);

  SemanticState &state;
  Scopes *scopes = nullptr;
  LoopBounds *loopBounds = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  LoopChecker *loopChecker = nullptr;
  LoopScanner *loopScanner = nullptr;
  MatchChecker *matchChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
  GenericWidening *genericWidening = nullptr;
  IterableChecker *iterableChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
