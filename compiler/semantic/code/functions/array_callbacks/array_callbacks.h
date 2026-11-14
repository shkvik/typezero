#pragma once

#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

struct SignedCondition {
  const Expr *cond;
  bool isWantedTrue;
};

class ArrayCallbacks {
public:
  explicit ArrayCallbacks(SemanticState &owner);

  void afterReduceInit(const AfterReduceInit &task);
  void checkCallbackMethod(const CheckCallbackMethod &task);
  void finishCallbackMethod(const FinishCallbackMethod &task);
  void afterExtraCallbackArgs(const AfterExtraCallbackArgs &task);
  void checkArrayCallbackResult(const CheckArrayCallbackResult &task);

private:
  using DefinedVars = llvm::SmallPtrSet<const VarSymbol *, 4>;
  using ResultCallbackIsPassed = Result<size_t, CodeFunctionsRegistryCallbackGivenError>;
  using ResultNoExtraArguments = Result<size_t, CodeFunctionsRegistryExtraArgumentsError>;
  using ResultComparisonNumber = Result<const Type *, CodeFunctionsRegistryComparisonError>;
  using ResultCallbackHasValue = Result<const Type *, CodeFunctionsRegistryMappedValueError>;
  using ResultInitialValueType = Result<const Type *, CodeFunctionsRegistryInitialValueError>;

  void finishArrayReduce(const ArrayCallbackShape &shape);
  void rememberCallbackFacts(const ArrayCallbackShape &shape);
  void startReduceInit(const ArrayCallbackShape &shape, const Type *expected);
  void checkMappedResult(const ArrayCallbackShape &shape, const Type *result);
  void checkCompareResult(const ArrayCallbackShape &shape, const Type *result);
  void rememberByteFacts(const ArrayCallbackShape &shape, FunctionDecl *callback);
  void rememberIndexFacts(const ArrayCallbackShape &shape, FunctionDecl *callback);
  void startCallbackArgument(const ArrayCallbackShape &shape, const Type *expected);
  void walkExtraCallbackArgs(CallExpr *call, const MemberExpr *member, bool isReduce);


  bool isKeepingDefined(const Expr *callback) const;


  const Expr *unconvertedCondition(const Expr *cond) const;
  const Type *filteredArray(const ArrayCallbackShape &shape);
  const Type *mappedReturn(const ArrayCallbackShape &shape, const Type *expected) const;
  const Type *callbackReturn(const ArrayCallbackShape &shape, const Type *expected) const;


  DefinedVars definedBy(SignedCondition part, std::vector<DefinedVars> &answers) const;
  DefinedVars joinDefined(bool isAnd, bool isWantedTrue, DefinedVars left,
                          const DefinedVars &right) const;


  ResultComparisonNumber checkComparison(const Type *result) const;
  ResultCallbackIsPassed checkCallbackGiven(size_t count, llvm::StringRef name) const;
  ResultCallbackHasValue checkMappedValue(const Type *result, llvm::StringRef name) const;
  ResultInitialValueType checkInitialValue(const Type *accumulator, llvm::StringRef name) const;
  ResultNoExtraArguments checkExtraArguments(size_t count, size_t maxArgs, llvm::StringRef name,
                                             bool isReduce) const;


  Builtin callbackBuiltin(const MemberExpr *member) const;
  ArrayCallback callbackKind(const MemberExpr *member) const;
  NodeChildren<SignedCondition> signedParts(SignedCondition part) const;
  std::vector<const Type *> callbackParams(const ArrayCallbackShape &shape) const;
  Signature callbackSignature(const ArrayCallbackShape &shape, const Type *expected) const;
  std::optional<ElementCallback> literalElements(const ArrayLitExpr *literal, unsigned param);

  SemanticState &state;
};

}
