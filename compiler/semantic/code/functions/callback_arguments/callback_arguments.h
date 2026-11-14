#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/code/functions/function_rules/function_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CallbackArguments {
public:
  CallbackArguments(SemanticState &owner, FunctionRules &sharedFunctionRules);

  void leaveCallback();
  void afterCallbackValue(const AfterCallbackValue &task);
  void checkFunctionArgSlot(const CheckFunctionArgSlot &task);
  void afterLiteralCallback(const AfterLiteralCallback &task);

private:
  using ResultNothingIsDropped = Result<const Type *, CodeFunctionsRegistryResultKeptError>;
  using ResultWithoutRestParam = Result<const Type *, CodeFunctionsRegistryFixedParamsError>;
  using ResultAsyncCallbackOk = Result<const Expr *, CodeFunctionsRegistryAsyncCallbackError>;
  using ResultFunctionIsPassed = Result<const Type *, CodeFunctionsRegistryFunctionGivenError>;
  using ResultParameterMatches = Result<const Type *, CodeFunctionsRegistryCallbackParamError>;
  using ResultReturnedTypeFits = Result<const Type *, CodeFunctionsRegistryReturnMatchesError>;
  using ResultArgumentsSuffice = Result<const Type *, CodeFunctionsRegistryArgumentsEnoughError>;

  void startCallbackValue(Expr *arg, const Signature &expected);
  void checkPassedFunction(Expr *arg, const Signature &expected, unsigned primary,
                           llvm::StringRef method);
  void checkCallbackResult(Expr *arg, const Signature &expected, llvm::StringRef name,
                           llvm::StringRef method);


  bool reportCallbackParams(const Expr *arg, const Signature &expected, unsigned primary,
                            const FunctionDecl *known, llvm::StringRef name,
                            llvm::StringRef method);


  FunctionDecl *namedFunction(const Expr *expr) const;
  FunctionDecl *variableFunction(const VarSymbol *variable) const;


  ResultWithoutRestParam checkFixedParams(const Type *type, llvm::StringRef name) const;
  ResultFunctionIsPassed checkFunctionGiven(const Type *type, llvm::StringRef method) const;
  ResultNothingIsDropped checkResultKept(const Type *type, const Signature &expected,
                                         llvm::StringRef name, llvm::StringRef method) const;
  ResultReturnedTypeFits checkReturnMatches(const Type *type, const Signature &expected,
                                            llvm::StringRef name, llvm::StringRef method) const;
  ResultArgumentsSuffice checkArgumentsEnough(const Type *type, const Signature &expected,
                                              llvm::StringRef name, llvm::StringRef method) const;
  ResultParameterMatches checkCallbackParam(const Type *type, const Signature &expected,
                                            size_t index, unsigned primary,
                                            const FunctionDecl *known, llvm::StringRef name,
                                            llvm::StringRef method) const;


  FuncSymbol *findGenericFunction(const Expr *expr) const;
  ResultAsyncCallbackOk checkAsyncCallback(const Expr *arg, const Signature &expected,
                                           llvm::StringRef method) const;

  SemanticState &state;
  FunctionRules &functionRules;
};

}
