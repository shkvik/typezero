#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassBases;
class IntlChecker;
class SemanticParts;
class FlowChecker;
class TypeDeclarer;
class TypeResolver;
class ClassDeclarer;
class TypeRelations;
class ReturnInferrer;
class FunctionChecker;
class GeneratorChecker;
class MethodCallChecker;
class GenericInstantiator;

class IterableChecker {
public:
  using ResultIterableParam = Result<const Type *, IterableCheckerIterableParamError>;

  IterableChecker(SemanticState &owner, SemanticParts &all);

  Expr *userIterator(Expr *expr);
  Expr *asyncUserIterator(Expr *expr);
  const Type *asyncIterableType(const Type *type);
  Expr *iteratorOf(Expr *it);
  const Type *nextValueType(const Type *type);
  const Type *elementTypeOf(const Type *type);
  FuncSymbol *iterateFunction();
  const Type *iteratorResultType(const Type *value, const Type *returned) const;
  ResultIterableParam checkIterableParam(FunctionDecl *functionDecl, FuncSymbol *function,
                                         const TypeRef &ref);
  bool isIterableRef(const TypeRef &ref) const;

private:
  using ResultIteratorGiven = Result<const Expr *, IterableCheckerIteratorGivenError>;
  using ResultAsyncIterator = Result<const Expr *, IterableCheckerAsyncIteratorGivenError>;

  const Type *nextFunctionType(const Type *object);
  const Type *nextMethodType(const Type *object);
  bool isPlainNext(const FunctionDecl *functionDecl) const;
  const Type *nextResultValue(const Type *result) const;
  bool isIteratorResultShape(const Type *member) const;
  bool isFinishedResult(const Type *member) const;
  FuncSymbol *iterateTarget(FuncSymbol *iterate, const Type *type, const Type *value,
                            llvm::SMLoc loc);
  Expr *callSymbolMethod(Expr *expr, llvm::StringRef name);
  const Type *soleMethodReturn(const Type *type, llvm::StringRef name);
  const Type *addIterableTypeParam(FunctionDecl *functionDecl, FuncSymbol *function,
                                   const Type *element, const TypeRef &ref);
  ResultIteratorGiven checkIteratorGiven(const Type *type, const Expr *result) const;
  ResultAsyncIterator checkAsyncIteratorGiven(const Type *type, const Expr *result) const;

  SemanticState &state;
  ClassBases *classBases = nullptr;
  FlowChecker *flowChecker = nullptr;
  IntlChecker *intlChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
};

}
