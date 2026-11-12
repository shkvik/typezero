#pragma once

#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class FlowChecker;
class IndexProofs;
class TypeRelations;
class ClosureChecker;
class ConditionFacts;
class ConstantFolder;
class GenericChecker;
class DecimalChecker;
class NestedFunctions;
class PredicateChecker;
class GenericInstantiator;
class FunctionValueChecker;

struct SignedCondition {
  const Expr *cond;
  bool whenTrue;
};

class CallbackChecker {
public:
  CallbackChecker(SemanticState &owner, SemanticParts &all);

  bool keepsDefined(const Expr *callback) const;
  Expr *checkCallbackMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkFunctionArg(Expr *arg, const Signature &expected, unsigned primary,
                         llvm::StringRef method);

private:
  using ResultCallbackIsPassed = Result<size_t, CallbackCheckerCallbackGivenError>;
  using ResultNoExtraArguments = Result<size_t, CallbackCheckerExtraArgumentsError>;
  using ResultComparisonNumber = Result<const Type *, CallbackCheckerComparisonError>;
  using ResultNothingIsDropped = Result<const Type *, CallbackCheckerResultKeptError>;
  using ResultWithoutRestParam = Result<const Type *, CallbackCheckerFixedParamsError>;
  using ResultCallbackHasValue = Result<const Type *, CallbackCheckerMappedValueError>;
  using ResultInitialValueType = Result<const Type *, CallbackCheckerInitialValueError>;
  using ResultFunctionIsPassed = Result<const Type *, CallbackCheckerFunctionGivenError>;
  using ResultParameterMatches = Result<const Type *, CallbackCheckerCallbackParamError>;
  using ResultReturnedTypeFits = Result<const Type *, CallbackCheckerReturnMatchesError>;
  using ResultArgumentsSuffice = Result<const Type *, CallbackCheckerArgumentsEnoughError>;

  Expr *checkLiteralCallback(FunctionExpr *functionExpr, const Signature &expected,
                             llvm::StringRef method);
  Expr *checkCallbackValue(Expr *arg, const Signature &expected);
  Expr *checkPassedFunction(Expr *arg, const Signature &expected, unsigned primary,
                            llvm::StringRef method);
  bool callbackParamsFit(const Expr *arg, const Signature &expected, unsigned primary,
                         const FunctionDecl *known, llvm::StringRef name, llvm::StringRef method);
  Expr *checkCallbackResult(Expr *arg, const Signature &expected, llvm::StringRef name,
                            llvm::StringRef method);
  ResultComparisonNumber comparison(const Type *result) const;
  ResultCallbackIsPassed callbackGiven(size_t count, llvm::StringRef name) const;
  ResultWithoutRestParam fixedParams(const Type *type, llvm::StringRef name) const;
  ResultCallbackHasValue mappedValue(const Type *result, llvm::StringRef name) const;
  ResultFunctionIsPassed functionGiven(const Type *type, llvm::StringRef method) const;
  ResultInitialValueType initialValue(const Type *accumulator, llvm::StringRef name) const;
  ResultNoExtraArguments extraArguments(size_t count, size_t maxArgs, llvm::StringRef name,
                                        bool reduce) const;
  ResultNothingIsDropped resultKept(const Type *type, const Signature &expected,
                                    llvm::StringRef name, llvm::StringRef method) const;
  ResultReturnedTypeFits returnMatches(const Type *type, const Signature &expected,
                                       llvm::StringRef name, llvm::StringRef method) const;
  ResultArgumentsSuffice argumentsEnough(const Type *type, const Signature &expected,
                                         llvm::StringRef name, llvm::StringRef method) const;
  using DefinedVars = llvm::SmallPtrSet<const VarSymbol *, 4>;

  NodeChildren<SignedCondition> signedParts(SignedCondition part) const;
  DefinedVars joinDefined(bool isAnd, bool whenTrue, DefinedVars left,
                          const DefinedVars &right) const;
  DefinedVars definedBy(SignedCondition part, std::vector<DefinedVars> &answers) const;

  ResultParameterMatches callbackParam(const Type *type, const Signature &expected, size_t index,
                                       unsigned primary, const FunctionDecl *known,
                                       llvm::StringRef name, llvm::StringRef method) const;

  struct ArrayCallbackShape {
    CallExpr *call;
    MemberExpr *member;
    const Type *arrayType;
    const Type *element;
    ArrayCallback kind;
    bool typed;
    size_t count;
    const Type *accumulator;
    bool someValue;
    bool isBoolean;
  };

  void walkExtraCallbackArgs(ArrayCallbackShape &shape);
  const Type *reduceInitType(const ArrayCallbackShape &shape);
  Signature callbackSignature(const ArrayCallbackShape &shape, const Type *expected) const;
  const Type *mappedReturn(const ArrayCallbackShape &shape, const Type *expected) const;
  std::vector<const Type *> callbackParams(const ArrayCallbackShape &shape) const;
  void rememberCallbackFacts(const ArrayCallbackShape &shape);
  std::optional<ElementCallback> literalElements(const ArrayLitExpr *literal, unsigned param);
  Expr *checkArrayCallbackResult(const ArrayCallbackShape &shape);
  Expr *checkMappedResult(const ArrayCallbackShape &shape, const Type *result);
  const Type *filteredArray(const ArrayCallbackShape &shape);
  Expr *finishArrayReduce(const ArrayCallbackShape &shape);
  Expr *checkCompareResult(const ArrayCallbackShape &shape, const Type *result);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  ConditionFacts *conditionFacts = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  NestedFunctions *nestedFunctions = nullptr;
  PredicateChecker *predicateChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
};

}
