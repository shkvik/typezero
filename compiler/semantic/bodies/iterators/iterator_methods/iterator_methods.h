#pragma once

#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
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
class GenericChecker;
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
  using ResultCallbackIsPassed = Result<size_t, IteratorMethodsCallbackGivenError>;
  using ResultNextValueOneType = Result<const Type *, IteratorMethodsNextValueError>;
  using ResultInitialValueType = Result<const Type *, IteratorMethodsInitialValueError>;
  using ResultFlatMapInnerType = Result<const Type *, IteratorMethodsFlatMapInnerError>;
  using ResultCallbackHasValue = Result<const Type *, IteratorMethodsCallbackValueError>;
  using ResultNotThrowOrReturn = Result<llvm::StringRef, IteratorMethodsPlainMethodError>;
  using ResultSourceIsIterable = Result<const Type *, IteratorMethodsIteratedSourceError>;
  using ResultKnownHelperFound = Result<Builtin, IteratorMethodsIteratorHelperError>;
  using ResultArrayStaticKnown = Result<const MemberExpr *, IteratorMethodsArrayStaticError>;

  ResultNextValueOneType nextValue(const Type *iterator) const;
  ResultFlatMapInnerType flatMapInner(const Type *result) const;
  ResultNotThrowOrReturn plainMethod(llvm::StringRef name) const;
  ResultInitialValueType initialValue(const Type *accumulator) const;
  ResultArrayStaticKnown arrayStatic(const MemberExpr *member) const;
  ResultSourceIsIterable iteratedSource(const Type *type, llvm::StringRef functionName) const;
  ResultCallbackHasValue callbackValue(const Type *result, llvm::StringRef functionName) const;
  ResultKnownHelperFound iteratorHelper(llvm::StringRef name, const Type *iterator) const;
  ResultCallbackIsPassed callbackGiven(size_t count, llvm::StringRef name, bool reduce) const;

  struct IteratorCallback {
    CallExpr *call;
    llvm::StringRef name;
    const Type *element;
    const Type *accumulator;
    bool isBoolean;
    size_t count;
  };

  bool hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min, unsigned max);
  Expr *checkIteratorStep(CallExpr *call, MemberExpr *member, Builtin builtin);
  std::optional<Builtin> helperBuiltin(CallExpr *call, MemberExpr *member);
  Expr *checkTakeDrop(CallExpr *call, MemberExpr *member, Builtin builtin);
  Expr *checkToArray(CallExpr *call, MemberExpr *member, Builtin builtin);
  Expr *checkIteratorCallback(CallExpr *call, MemberExpr *member, IteratorMethod method,
                              Builtin builtin);
  const Type *reduceInitType(CallExpr *call, const Type *element);
  Signature callbackSignature(IteratorMethod method, const Type *element,
                              const Type *accumulator) const;
  Expr *checkCallbackResult(const IteratorCallback &callback, IteratorMethod method);
  Expr *checkMapResult(const IteratorCallback &callback, const Type *result);
  Expr *checkFlatMapResult(CallExpr *call, const Type *result);
  const Type *filteredElement(const IteratorCallback &callback);
  Expr *finishReduce(const IteratorCallback &callback);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  MemberChecker *memberChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
