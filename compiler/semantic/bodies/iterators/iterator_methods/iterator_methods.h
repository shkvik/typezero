#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class DictChecker;
class ExprChecker;
class FlowChecker;
class IndexProofs;
class MemberChecker;
class TypeRelations;
class ArgumentChecker;
class CallbackChecker;
class GeneratorChecker;
class PredicateChecker;

class IteratorMethods {
public:
  using ResultIteratorFromCall = Result<Expr *, IteratorMethodsCheckIteratorFromError>;
  using ResultArrayIterateCall = Result<Expr *, IteratorMethodsCheckArrayIteratorError>;

  IteratorMethods(SemanticState &owner, SemanticParts &all);

  void markFused(Expr *expr) const;
  Expr *checkArrayFrom(CallExpr *call, MemberExpr *member);
  Expr *checkIteratorMethod(CallExpr *call, MemberExpr *member);
  ResultIteratorFromCall checkIteratorFrom(CallExpr *call, MemberExpr *member);
  ResultArrayIterateCall checkArrayIterator(CallExpr *call, MemberExpr *member);

private:
  struct Helper {
    llvm::StringLiteral name;
    Builtin builtin;
  };

  using ResultCallbackIsPassed = Result<size_t, IteratorMethodsCallbackGivenError>;
  using ResultNextValueOneType = Result<const Type *, IteratorMethodsNextValueError>;
  using ResultInitialValueType = Result<const Type *, IteratorMethodsInitialValueError>;
  using ResultFlatMapInnerType = Result<const Type *, IteratorMethodsFlatMapInnerError>;
  using ResultCallbackHasValue = Result<const Type *, IteratorMethodsCallbackValueError>;
  using ResultNotThrowOrReturn = Result<llvm::StringRef, IteratorMethodsPlainMethodError>;
  using ResultSourceIsIterable = Result<const Type *, IteratorMethodsIteratedSourceError>;
  using ResultKnownHelperFound = Result<const Helper *, IteratorMethodsIteratorHelperError>;
  using ResultArrayStaticKnown = Result<const MemberExpr *, IteratorMethodsArrayStaticError>;

  static ResultNextValueOneType nextValue(const Type *iterator);
  static ResultFlatMapInnerType flatMapInner(const Type *result);
  static ResultNotThrowOrReturn plainMethod(llvm::StringRef name);
  static ResultInitialValueType initialValue(const Type *accumulator);
  static ResultArrayStaticKnown arrayStatic(const MemberExpr *member);
  static ResultSourceIsIterable iteratedSource(const Type *type, llvm::StringRef fn);
  static ResultCallbackHasValue callbackValue(const Type *result, llvm::StringRef fn);
  static ResultKnownHelperFound iteratorHelper(llvm::StringRef name, const Type *iterator);
  static ResultCallbackIsPassed callbackGiven(size_t count, llvm::StringRef name, bool reduce);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  MemberChecker *memberChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
