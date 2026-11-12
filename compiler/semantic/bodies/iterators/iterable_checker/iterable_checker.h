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
  ResultIterableParam iterableParam(FunctionDecl *functionDecl, FuncSymbol *function,
                                    const TypeRef &ref);
  bool isIterableRef(const TypeRef &ref) const;

private:
  using ResultIteratorGiven = Result<const Expr *, IterableCheckerIteratorGivenError>;
  using ResultAsyncIterator = Result<const Expr *, IterableCheckerAsyncIteratorGivenError>;

  ResultIteratorGiven iteratorGiven(const Type *type, const Expr *result) const;
  ResultAsyncIterator asyncIteratorGiven(const Type *type, const Expr *result) const;

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
