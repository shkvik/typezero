#pragma once

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

  static ResultComparisonNumber comparison(const Type *result);
  static ResultCallbackIsPassed callbackGiven(size_t count, llvm::StringRef name);
  static ResultWithoutRestParam fixedParams(const Type *type, llvm::StringRef name);
  static ResultCallbackHasValue mappedValue(const Type *result, llvm::StringRef name);
  static ResultFunctionIsPassed functionGiven(const Type *type, llvm::StringRef method);
  static ResultInitialValueType initialValue(const Type *accumulator, llvm::StringRef name);
  static ResultNoExtraArguments extraArguments(size_t count, size_t maxArgs, llvm::StringRef name,
                                               bool reduce);
  static ResultNothingIsDropped resultKept(const Type *type, const Signature &expected,
                                           llvm::StringRef name, llvm::StringRef method);
  static ResultReturnedTypeFits returnMatches(const Type *type, const Signature &expected,
                                              llvm::StringRef name, llvm::StringRef method);
  static ResultArgumentsSuffice argumentsEnough(const Type *type, const Signature &expected,
                                                llvm::StringRef name, llvm::StringRef method);
  using DefinedVars = llvm::SmallPtrSet<const VarSymbol *, 4>;

  static NodeChildren<SignedCondition> signedParts(SignedCondition part);
  static DefinedVars joinDefined(bool isAnd, bool whenTrue, DefinedVars left,
                                 const DefinedVars &right);
  DefinedVars definedBy(SignedCondition part, std::vector<DefinedVars> &answers) const;

  static ResultParameterMatches callbackParam(const Type *type, const Signature &expected, size_t i,
                                              unsigned primary, const FunctionDecl *known,
                                              llvm::StringRef name, llvm::StringRef method);

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
  NestedFunctions *nestedFunctions = nullptr;
  PredicateChecker *predicateChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
};

}
