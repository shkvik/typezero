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

  void walkForAwait(ForOfStmt *stmt);
  bool isMappingCall(const Expr *expr) const;
  bool isTakingElement(const Expr *expr) const;
  static const Type *awaitedType(const Type *type);
  Expr *checkAwait(AwaitExpr *await, const Type *expected);
  ResultPromiseTypeRef resolvePromiseType(const TypeRef &ref);
  const Type *asyncReturnType(const FunctionDecl *functionDecl, const Type *declared);
  void finishAsyncReturns(FuncSymbol *function, llvm::ArrayRef<ReturnStmt *> returns);
  ResultPromiseHandled checkDiscardedPromise(const Expr *expr, bool isCallback, llvm::SMLoc at);
  const Type *inferAsyncType(const FuncSymbol *function, llvm::ArrayRef<ReturnStmt *> returns,
                             bool isFallingOff, ReturnErrors errors);

private:
  enum class Returned {
    NOTHING,
    UNKNOWN,
    VALUE,
  };

  struct SortedReturns {
    std::vector<const ReturnStmt *> valued;
    bool hasNothing;
    bool hasUnknown;
  };

  using ResultAwaitedOneType = Result<const Type *, AsyncCheckerAwaitableError>;
  using ResultReturnsOneType = Result<const Type *, AsyncCheckerReturnsJoinError>;
  using ResultBindingPattern = Result<const Type *, AsyncCheckerBindingShapeError>;
  using ResultPromiseMatches = Result<const Type *, AsyncCheckerPromiseMatchesError>;
  using ResultAwaitedElement = Result<const Type *, AsyncCheckerAwaitedElementError>;
  using ResultReturnsPromise = Result<const Type *, AsyncCheckerPromiseDeclaredError>;
  using ResultInAsyncContext = Result<const FuncSymbol *, AsyncCheckerAsyncContextError>;
  using ResultBindingUntyped = Result<const VarDeclarator *, AsyncCheckerBindingTypeError>;

  ResultInAsyncContext awaitContext() const;
  ResultInAsyncContext forAwaitContext() const;
  ResultAwaitedOneType awaitable(const Type *type) const;
  ResultAwaitedElement awaitedElement(const Type *type) const;
  ResultBindingUntyped bindingType(const VarDeclarator &binding) const;
  ResultReturnsPromise promiseDeclared(const FunctionDecl *functionDecl,
                                       const Type *declared) const;
  ResultBindingPattern bindingShape(const VarDeclarator &binding, const Type *element) const;
  ResultReturnsOneType returnsJoin(const Type *type, const Type *candidate,
                                   const FuncSymbol *function) const;
  ResultPromiseMatches promiseMatches(const Type *given, const Type *value,
                                      const FuncSymbol *function) const;

  int depthChange(char symbol) const;
  Expr *asyncSource(Expr *iterable);
  Expr *checkedIterable(Expr *iterable);
  void walkSendsNothing(const Expr *iterable);
  const Type *loopElement(const Expr *iterable);
  static bool hasPromiseMember(const Type *type);
  std::string statementText(llvm::SMLoc at) const;
  static const Type *awaitedPlain(const Type *type);
  const Type *withoutPromise(const Type *type) const;
  bool isOptionalAsyncIterable(const Type *type) const;
  bool isPlainFunction(const FuncSymbol *function) const;
  void walkAwaitBody(ForOfStmt *stmt, const Type *element);
  Returned returnedKind(const ReturnStmt *returnStmt) const;
  std::string asyncHint(const FunctionDecl *functionDecl) const;
  void walkContext(ResultInAsyncContext context, llvm::SMLoc loc);
  SortedReturns sortedReturns(llvm::ArrayRef<ReturnStmt *> returns) const;
  const Type *promiseHint(const Expr *value, const Type *expected) const;
  const Type *boundElement(const VarDeclarator &binding, const Type *element);
  void finishAsyncReturn(const FuncSymbol *function, ReturnStmt *returnStmt, const Type *value);

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
