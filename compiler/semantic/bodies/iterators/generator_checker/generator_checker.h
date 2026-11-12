#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class DictChecker;
class ExprChecker;
class FlowChecker;
class TypeDeclarer;
class TypeResolver;
class TypeRelations;
class IteratorMethods;
class IterableChecker;

class GeneratorChecker {
public:
  using ResultNextNeedsNoValue = Result<const Type *, GeneratorCheckerCheckSendsNothingError>;
  using ResultResolvedIterator = Result<const Type *, GeneratorCheckerResolveIteratorTypeError>;
  using ResultGeneratorReturns = Result<const Type *, GeneratorCheckerGeneratorReturnTypeError>;

  GeneratorChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkYield(YieldExpr *yield);
  const Type *asyncIteratedType(const Type *type);
  const Type *iteratedType(const Type *type) const;
  ResultResolvedIterator resolveIteratorType(const TypeRef &ref);
  Expr *destructuredIterator(Expr *init, const VarDeclarator &declarator);
  ResultNextNeedsNoValue checkSendsNothing(const Type *iterator, llvm::StringRef what);
  ResultGeneratorReturns checkGeneratorReturnType(const FunctionDecl *functionDecl,
                                                  const Type *declared);
  const Type *inferGeneratorType(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields,
                                 llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                 ReturnErrors errors);

private:
  using ResultNothingSentAsync = Result<const Type *, GeneratorCheckerAsyncSentError>;
  using ResultYieldedTypesJoin = Result<const Type *, GeneratorCheckerYieldsJoinError>;
  using ResultReturnsOfOneType = Result<const Type *, GeneratorCheckerReturnsJoinError>;
  using ResultYieldedTypeKnown = Result<const Type *, GeneratorCheckerYieldsKnownError>;
  using ResultPromiseYieldFits = Result<const Type *, GeneratorCheckerPromiseYieldError>;
  using ResultYieldGivesAValue = Result<const Type *, GeneratorCheckerValueYieldedError>;
  using ResultDelegateSyncFits = Result<const Type *, GeneratorCheckerDelegateSyncError>;
  using ResultSentTypeDeclared = Result<const Type *, GeneratorCheckerSentDeclaredError>;
  using ResultDelegateAsyncFit = Result<const Type *, GeneratorCheckerDelegateAsyncError>;

  struct YieldShape {
    FuncSymbol *function;
    bool isAsync;
    const Type *yielded;
    const Type *sent;
    bool isLoose;
  };

  struct YieldJoin {
    const Type *yielded;
    bool isBroken;
    bool hasFailed;
  };

  struct KnownYield {
    const YieldExpr *yield;
    const Type *type;
  };

  struct KnownReturn {
    const ReturnStmt *returnStmt;
    const Type *type;
  };

  ResultResolvedIterator resolveIteratorResult(const TypeRef &ref);
  ResultResolvedIterator resolveIteratorArgs(const TypeRef &ref, bool isAsync);
  const Type *typeArgOr(const TypeRef &ref, size_t index, const Type *fallback);
  bool isNamingNoValue(const TypeRef &arg) const;
  std::string shownElementName(const Type *element) const;
  std::string shownYieldedName(const Type *yielded) const;
  ResultYieldedTypesJoin checkYieldsJoin(const Type *yielded, const Type *type,
                                         const FuncSymbol *function) const;
  ResultReturnsOfOneType checkReturnsJoin(const Type *returned, const Type *type,
                                          const FuncSymbol *function) const;
  ResultYieldedTypeKnown checkYieldsKnown(const Type *yielded, const FuncSymbol *function,
                                          bool hasNoYields) const;
  std::string nothingYieldedReason(const Type *yielded, bool hasNoYields) const;
  YieldJoin joinYields(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields, bool isAsync,
                       ReturnErrors errors);
  std::vector<KnownYield> knownYields(llvm::ArrayRef<YieldExpr *> yields, bool isAsync) const;
  const Type *yieldedValueType(const YieldExpr *yield, bool isAsync) const;
  const Type *unknownYields(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields,
                            const YieldJoin &join, ReturnErrors errors);
  std::optional<const Type *> joinedReturns(const FuncSymbol *function,
                                            llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                            bool isAsync, ReturnErrors errors);
  bool hasUndefinedReturn(llvm::ArrayRef<ReturnStmt *> returns, bool isAsync) const;
  std::vector<KnownReturn> knownReturns(llvm::ArrayRef<ReturnStmt *> returns, bool isAsync) const;
  const Type *returnedValueType(const ReturnStmt *returnStmt, bool isAsync) const;
  ResultNothingSentAsync checkAsyncSent(const Type *sent, const Type *yielded, bool isLoose) const;
  ResultPromiseYieldFits checkPromiseYield(const Type *type, const Type *given,
                                           const YieldShape &shape) const;
  ResultYieldGivesAValue checkValueYielded(const YieldShape &shape) const;
  ResultDelegateAsyncFit checkDelegateAsync(const Type *type, const Type *element,
                                            const YieldShape &shape) const;
  ResultDelegateSyncFits checkDelegateSync(const Type *type, const Type *element,
                                           const YieldShape &shape) const;
  ResultSentTypeDeclared checkSentDeclared(const YieldExpr *yield, const YieldShape &shape) const;
  Expr *checkStrayYield(YieldExpr *yield);
  YieldShape yieldShape(const YieldExpr *yield, FuncSymbol *function);
  bool isIteratorKind(const Type *type, bool isAsync) const;
  const Type *asyncSentType(const YieldExpr *yield, const Type *sent, const Type *yielded,
                            bool isLoose);
  void walkYieldValue(YieldExpr *yield, const YieldShape &shape);
  void walkEmptyYield(const YieldExpr *yield, const YieldShape &shape);
  void walkAsyncYieldValue(YieldExpr *yield, const YieldShape &shape);
  void walkDelegate(YieldExpr *yield, const YieldShape &shape);
  void walkAsyncDelegate(YieldExpr *yield, const YieldShape &shape);
  void walkSyncDelegate(YieldExpr *yield, const YieldShape &shape);
  const Type *yieldResultType(const YieldExpr *yield, const YieldShape &shape) const;
  const Type *delegateResultType(const YieldExpr *yield, const YieldShape &shape) const;
  Expr *takeCall(Expr *init, const VarDeclarator &declarator);

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  IterableChecker *iterableChecker = nullptr;
};

}
