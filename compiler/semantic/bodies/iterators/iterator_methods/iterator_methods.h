#pragma once

#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
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
struct ArrayMethodInfo;

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

  struct IteratorCallback {
    CallExpr *call;
    llvm::StringRef name;
    const Type *element;
    const Type *accumulator;
    bool isBoolean;
    size_t count;
  };

  Expr *markStageFused(CallExpr *call) const;
  Builtin arrayIteratorBuiltin(const ArrayMethodInfo *method) const;
  const Type *arrayIteratorType(const ArrayMethodInfo *method, const Type *element) const;
  ResultSourceIsIterable checkIteratedSource(const Type *type, llvm::StringRef functionName) const;
  IteratorMethodsCheckIteratorFromErrorCode
  iteratorFromCode(IteratorMethodsIteratedSourceErrorCode code) const;
  ResultArrayStaticKnown checkArrayStatic(const MemberExpr *member) const;
  ResultCallbackHasValue checkCallbackValue(const Type *result, llvm::StringRef functionName) const;
  Expr *reportArrayFromSource(CallExpr *call, const IteratorMethodsIteratedSourceError &error);
  void walkArrayFromSends(const CallExpr *call, const Type *type);
  Expr *checkArrayFromMapper(CallExpr *call, const Type *element);
  ResultNextValueOneType checkNextValue(const Type *iterator) const;
  ResultNotThrowOrReturn checkPlainMethod(llvm::StringRef name) const;
  ResultKnownHelperFound checkIteratorHelper(llvm::StringRef name, const Type *iterator) const;
  ResultCallbackIsPassed checkCallbackGiven(size_t count, llvm::StringRef name,
                                            bool isReduce) const;
  ResultInitialValueType checkInitialValue(const Type *accumulator) const;
  ResultFlatMapInnerType checkFlatMapInner(const Type *result) const;
  bool reportWrongArity(CallExpr *call, MemberExpr *member, unsigned min, unsigned max);
  Expr *checkIteratorStep(CallExpr *call, MemberExpr *member, Builtin builtin);
  void walkSentArgument(CallExpr *call, const Type *iterator);
  const Type *stepResultType(const Type *value) const;
  std::optional<Builtin> helperBuiltin(CallExpr *call, MemberExpr *member);
  Expr *checkTakeDrop(CallExpr *call, MemberExpr *member, Builtin builtin);
  Expr *checkToArray(CallExpr *call, MemberExpr *member, Builtin builtin);
  Expr *checkIteratorCallback(CallExpr *call, MemberExpr *member, IteratorMethod method,
                              Builtin builtin);
  void walkCallbackArgument(CallExpr *call, const Signature &signature, IteratorMethod method,
                            Builtin builtin, llvm::StringRef name);
  const Type *reduceInitType(CallExpr *call, const Type *element);
  Signature callbackSignature(IteratorMethod method, const Type *element,
                              const Type *accumulator) const;
  const Type *callbackReturn(IteratorMethod method, const Type *accumulator) const;
  Expr *checkCallbackResult(const IteratorCallback &callback, IteratorMethod method);
  Expr *checkFilterResult(const IteratorCallback &callback);
  Expr *checkMapResult(const IteratorCallback &callback, const Type *result);
  Expr *checkFlatMapResult(const IteratorCallback &callback, const Type *result);
  bool reportFlatSends(const IteratorCallback &callback, const Type *result);
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
