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
  bool isWantedTrue;
};

class CallbackChecker {
public:
  CallbackChecker(SemanticState &owner, SemanticParts &all);

  bool isKeepingDefined(const Expr *callback) const;
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
  using DefinedVars = llvm::SmallPtrSet<const VarSymbol *, 4>;

  struct ArrayCallbackShape {
    CallExpr *call;
    MemberExpr *member;
    const Type *arrayType;
    const Type *element;
    ArrayCallback kind;
    bool isTyped;
    size_t count;
    const Type *accumulator;
    bool hasSomeValue;
    bool isBoolean;
  };

  ResultFunctionIsPassed checkFunctionGiven(const Type *type, llvm::StringRef method) const;
  ResultWithoutRestParam checkFixedParams(const Type *type, llvm::StringRef name) const;
  ResultParameterMatches checkCallbackParam(const Type *type, const Signature &expected,
                                            size_t index, unsigned primary,
                                            const FunctionDecl *known, llvm::StringRef name,
                                            llvm::StringRef method) const;
  ResultArgumentsSuffice checkArgumentsEnough(const Type *type, const Signature &expected,
                                              llvm::StringRef name, llvm::StringRef method) const;
  ResultReturnedTypeFits checkReturnMatches(const Type *type, const Signature &expected,
                                            llvm::StringRef name, llvm::StringRef method) const;
  ResultNothingIsDropped checkResultKept(const Type *type, const Signature &expected,
                                         llvm::StringRef name, llvm::StringRef method) const;
  Expr *checkLiteralCallback(FunctionExpr *functionExpr, const Signature &expected,
                             llvm::StringRef method);
  Expr *checkCallbackValue(Expr *arg, const Signature &expected);
  Expr *checkPassedFunction(Expr *arg, const Signature &expected, unsigned primary,
                            llvm::StringRef method);
  bool reportCallbackParams(const Expr *arg, const Signature &expected, unsigned primary,
                            const FunctionDecl *known, llvm::StringRef name,
                            llvm::StringRef method);
  Expr *checkCallbackResult(Expr *arg, const Signature &expected, llvm::StringRef name,
                            llvm::StringRef method);
  NodeChildren<SignedCondition> signedParts(SignedCondition part) const;
  DefinedVars definedBy(SignedCondition part, std::vector<DefinedVars> &answers) const;
  const Expr *unconvertedCondition(const Expr *cond) const;
  DefinedVars joinDefined(bool isAnd, bool isWantedTrue, DefinedVars left,
                          const DefinedVars &right) const;
  ResultCallbackIsPassed checkCallbackGiven(size_t count, llvm::StringRef name) const;
  ResultNoExtraArguments checkExtraArguments(size_t count, size_t maxArgs, llvm::StringRef name,
                                             bool isReduce) const;
  ResultInitialValueType checkInitialValue(const Type *accumulator, llvm::StringRef name) const;
  ResultCallbackHasValue checkMappedValue(const Type *result, llvm::StringRef name) const;
  ResultComparisonNumber checkComparison(const Type *result) const;
  void walkExtraCallbackArgs(CallExpr *call, const MemberExpr *member, bool isReduce);
  const Type *reduceInitType(CallExpr *call, const MemberExpr *member, const Type *element);
  Signature callbackSignature(const ArrayCallbackShape &shape, const Type *expected) const;
  const Type *callbackReturn(const ArrayCallbackShape &shape, const Type *expected) const;
  const Type *mappedReturn(const ArrayCallbackShape &shape, const Type *expected) const;
  std::vector<const Type *> callbackParams(const ArrayCallbackShape &shape) const;
  void rememberCallbackFacts(const ArrayCallbackShape &shape);
  void rememberIndexFacts(const ArrayCallbackShape &shape, FunctionDecl *callback);
  void rememberByteFacts(const ArrayCallbackShape &shape, FunctionDecl *callback);
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
