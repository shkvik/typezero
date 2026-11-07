#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/iterators/iterator_methods/iterator_methods_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class IteratorMethods {
public:
  using ResultIteratorFromCall = Result<Expr *, IteratorMethodsCheckIteratorFromError>;
  using ResultArrayIterateCall = Result<Expr *, IteratorMethodsCheckArrayIteratorError>;

  explicit IteratorMethods(CheckState &owner);

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

  CheckState &state;
};

}
