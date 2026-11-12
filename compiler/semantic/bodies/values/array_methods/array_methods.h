#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/array_methods/array_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class JsonChecker;
class SemanticParts;
class ExprChecker;
class FlowChecker;
class IndexProofs;
class MemberChecker;
class TypeRelations;
class GenericChecker;
class UnknownChecker;
class ArgumentChecker;
class CallbackChecker;
class IteratorMethods;
struct ArrayMethodInfo;

class ArrayMethods {
public:
  using ResultArrayMethodCall = Result<Expr *, ArrayMethodsCheckArrayMethodError>;

  ArrayMethods(SemanticState &owner, SemanticParts &all);

  ResultArrayMethodCall checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultSortableElement = Result<const Type *, ArrayMethodsSortableError>;
  using ResultElementsAreText = Result<const Type *, ArrayMethodsTextElementsError>;

  struct ArrayCall {
    CallExpr *call;
    MemberExpr *member;
    const Type *expected;
    const Type *arrayType;
    const Type *element;
    const VarSymbol *self;
    size_t count;
    const ArrayMethodInfo *method;
  };

  const VarSymbol *arrayVariable(const Expr *object) const;
  ResultElementsAreText checkTextElements(const Type *element, llvm::StringRef name) const;
  ResultSortableElement checkSortable(const Type *element) const;
  std::optional<ArrayMethodsSortableErrorCode> sortProblem(const Type *element) const;
  bool areAllTextual(llvm::ArrayRef<const Type *> members) const;
  std::string sortMessage(ArrayMethodsSortableErrorCode code, const Type *element) const;
  ResultArrayMethodCall checkArity(const ArrayCall &arrayCall, unsigned min, unsigned max);
  ResultArrayMethodCall checkTextArity(const ArrayCall &arrayCall, unsigned max);
  void walkElementArg(const ArrayCall &arrayCall, size_t index);
  void walkPositionArgs(const ArrayCall &arrayCall, size_t from);
  void finishCall(const ArrayCall &arrayCall, const Type *type);
  ResultArrayMethodCall checkCallbackCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkIteratorCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkGrowingCall(const ArrayCall &arrayCall);
  void walkPushedArg(const ArrayCall &arrayCall, size_t index);
  ResultArrayMethodCall checkShrinkingCall(const ArrayCall &arrayCall);
  void shrinkLength(const VarSymbol *self, bool isNotEmpty);
  bool isLengthFact(const VarSymbol *self, const Facts::Fact &fact) const;
  ResultArrayMethodCall checkAtCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkSliceCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkConcatCall(const ArrayCall &arrayCall);
  void walkConcatArg(const ArrayCall &arrayCall, size_t index);
  ResultArrayMethodCall checkSearchCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkJoinCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkToStringCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkReverseCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkFillCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkSortCall(const ArrayCall &arrayCall);
  ResultArrayMethodCall checkSpliceCall(const ArrayCall &arrayCall);
  void walkSplicedArg(const ArrayCall &arrayCall, size_t index);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  JsonChecker *jsonChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
  UnknownChecker *unknownChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
};

}
