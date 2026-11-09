#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FlowChecker;
class TypeDeclarer;
class TypeResolver;
class ClassDeclarer;
class TypeRelations;
class ReturnInferrer;
class FunctionChecker;
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
  static const Type *iteratorResultType(const Type *value, const Type *returned);
  ResultIterableParam iterableParam(FunctionDecl *fn, FuncSymbol *sym, const TypeRef &ref);
  bool isIterableRef(const TypeRef &ref) const;

private:
  using ResultIteratorGiven = Result<const Expr *, IterableCheckerIteratorGivenError>;
  using ResultAsyncIterator = Result<const Expr *, IterableCheckerAsyncIteratorGivenError>;

  static ResultIteratorGiven iteratorGiven(const Type *type, const Expr *result);
  static ResultAsyncIterator asyncIteratorGiven(const Type *type, const Expr *result);

  SemanticState &state;
  FlowChecker *flowChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
};

}
