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

class GeneratorChecker {
public:
  using ResultNextNeedsNoValue = Result<const Type *, GeneratorCheckerCheckSendsNothingError>;
  using ResultResolvedIterator = Result<const Type *, GeneratorCheckerResolveIteratorTypeError>;
  using ResultGeneratorReturns = Result<const Type *, GeneratorCheckerGeneratorReturnTypeError>;

  GeneratorChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkYield(YieldExpr *yield);
  const Type *asyncIteratedType(const Type *type);
  static const Type *iteratedType(const Type *type);
  ResultResolvedIterator resolveIteratorType(const TypeRef &ref);
  Expr *destructuredIterator(Expr *init, const VarDeclarator &d);
  ResultNextNeedsNoValue checkSendsNothing(const Type *iterator, llvm::StringRef what);
  ResultGeneratorReturns generatorReturnType(const FunctionDecl *fn, const Type *declared);
  const Type *inferGeneratorType(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields,
                                 llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff, bool report);

private:
  static bool namesNoValue(const TypeRef &arg);
  using ResultNothingSentAsync = Result<const Type *, GeneratorCheckerAsyncSentError>;
  using ResultYieldedTypesJoin = Result<const Type *, GeneratorCheckerYieldsJoinError>;
  using ResultReturnsOfOneType = Result<const Type *, GeneratorCheckerReturnsJoinError>;
  using ResultYieldedTypeKnown = Result<const Type *, GeneratorCheckerYieldsKnownError>;
  using ResultPromiseYieldFits = Result<const Type *, GeneratorCheckerPromiseYieldError>;
  using ResultYieldGivesAValue = Result<const Type *, GeneratorCheckerValueYieldedError>;
  using ResultDelegateSyncFits = Result<const Type *, GeneratorCheckerDelegateSyncError>;
  using ResultSentTypeDeclared = Result<const Type *, GeneratorCheckerSentDeclaredError>;
  using ResultDelegateAsyncFit = Result<const Type *, GeneratorCheckerDelegateAsyncError>;

  static ResultYieldGivesAValue valueYielded(const Type *yielded, const FuncSymbol *fn);
  static ResultNothingSentAsync asyncSent(const Type *sent, const Type *yielded, bool loose);
  static ResultYieldedTypesJoin yieldsJoin(const Type *yielded, const Type *t,
                                           const FuncSymbol *fn);
  static ResultYieldedTypeKnown yieldsKnown(const Type *yielded, const FuncSymbol *fn,
                                            bool noYields);
  static ResultReturnsOfOneType returnsJoin(const Type *returned, const Type *t,
                                            const FuncSymbol *fn);
  static ResultPromiseYieldFits promiseYield(const Type *t, const Type *given, const Type *yielded,
                                             const FuncSymbol *fn);
  static ResultDelegateAsyncFit delegateAsync(const Type *type, const Type *element,
                                              const Type *yielded, const FuncSymbol *fn);
  static ResultSentTypeDeclared sentDeclared(const YieldExpr *yield, bool async, const Type *sent,
                                             const Type *yielded, bool loose);
  static ResultDelegateSyncFits delegateSync(const Type *type, const Type *element,
                                             const Type *yielded, const Type *sent, bool loose,
                                             const FuncSymbol *fn);

  struct YieldShape {
    FuncSymbol *fn;
    bool async;
    const Type *yielded;
    const Type *sent;
    bool loose;
  };

  YieldShape yieldShape(const YieldExpr *yield, FuncSymbol *fn);
  ResultResolvedIterator resolveIteratorResult(const TypeRef &ref);
  ResultResolvedIterator resolveIteratorArgs(const TypeRef &ref, bool async);
  const Type *typeArgOr(const TypeRef &ref, size_t i, const Type *fallback);

  struct YieldJoin {
    const Type *yielded;
    bool broken;
    bool failed;
  };

  YieldJoin joinYields(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields, bool async,
                       bool report);
  static const Type *yieldedValueType(const YieldExpr *yield, bool async);
  const Type *unknownYields(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields,
                            const YieldJoin &join, bool report);
  std::optional<const Type *> joinedReturns(const FuncSymbol *fn,
                                            llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff,
                                            bool async, bool report);
  void walkYieldValue(YieldExpr *yield, const YieldShape &shape);
  void walkAsyncYieldValue(YieldExpr *yield, const YieldShape &shape);
  void walkDelegate(YieldExpr *yield, const YieldShape &shape);
  static const Type *yieldResultType(const YieldExpr *yield, const YieldShape &shape);

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
};

}
