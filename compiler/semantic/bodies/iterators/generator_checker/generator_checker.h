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
  ResultGeneratorReturns generatorReturnType(const FunctionDecl *functionDecl,
                                             const Type *declared);
  const Type *inferGeneratorType(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields,
                                 llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                 ReturnErrors errors);

private:
  bool namesNoValue(const TypeRef &arg) const;
  using ResultNothingSentAsync = Result<const Type *, GeneratorCheckerAsyncSentError>;
  using ResultYieldedTypesJoin = Result<const Type *, GeneratorCheckerYieldsJoinError>;
  using ResultReturnsOfOneType = Result<const Type *, GeneratorCheckerReturnsJoinError>;
  using ResultYieldedTypeKnown = Result<const Type *, GeneratorCheckerYieldsKnownError>;
  using ResultPromiseYieldFits = Result<const Type *, GeneratorCheckerPromiseYieldError>;
  using ResultYieldGivesAValue = Result<const Type *, GeneratorCheckerValueYieldedError>;
  using ResultDelegateSyncFits = Result<const Type *, GeneratorCheckerDelegateSyncError>;
  using ResultSentTypeDeclared = Result<const Type *, GeneratorCheckerSentDeclaredError>;
  using ResultDelegateAsyncFit = Result<const Type *, GeneratorCheckerDelegateAsyncError>;

  ResultYieldGivesAValue valueYielded(const Type *yielded, const FuncSymbol *function) const;
  ResultNothingSentAsync asyncSent(const Type *sent, const Type *yielded, bool loose) const;
  ResultYieldedTypesJoin yieldsJoin(const Type *yielded, const Type *type,
                                    const FuncSymbol *function) const;
  ResultYieldedTypeKnown yieldsKnown(const Type *yielded, const FuncSymbol *function,
                                     bool noYields) const;
  ResultReturnsOfOneType returnsJoin(const Type *returned, const Type *type,
                                     const FuncSymbol *function) const;
  ResultPromiseYieldFits promiseYield(const Type *type, const Type *given, const Type *yielded,
                                      const FuncSymbol *function) const;
  ResultDelegateAsyncFit delegateAsync(const Type *type, const Type *element, const Type *yielded,
                                       const FuncSymbol *function) const;
  ResultSentTypeDeclared sentDeclared(const YieldExpr *yield, bool async, const Type *sent,
                                      const Type *yielded, bool loose) const;
  ResultDelegateSyncFits delegateSync(const Type *type, const Type *element, const Type *yielded,
                                      const Type *sent, bool loose,
                                      const FuncSymbol *function) const;

  struct YieldShape {
    FuncSymbol *fn;
    bool async;
    const Type *yielded;
    const Type *sent;
    bool loose;
  };

  YieldShape yieldShape(const YieldExpr *yield, FuncSymbol *function);
  ResultResolvedIterator resolveIteratorResult(const TypeRef &ref);
  ResultResolvedIterator resolveIteratorArgs(const TypeRef &ref, bool async);
  const Type *typeArgOr(const TypeRef &ref, size_t index, const Type *fallback);

  struct YieldJoin {
    const Type *yielded;
    bool broken;
    bool failed;
  };

  YieldJoin joinYields(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields, bool async,
                       ReturnErrors errors);
  const Type *yieldedValueType(const YieldExpr *yield, bool async) const;
  const Type *unknownYields(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields,
                            const YieldJoin &join, ReturnErrors errors);
  std::optional<const Type *> joinedReturns(const FuncSymbol *function,
                                            llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                            bool async, ReturnErrors errors);
  void walkYieldValue(YieldExpr *yield, const YieldShape &shape);
  void walkAsyncYieldValue(YieldExpr *yield, const YieldShape &shape);
  void walkDelegate(YieldExpr *yield, const YieldShape &shape);
  const Type *yieldResultType(const YieldExpr *yield, const YieldShape &shape) const;

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
