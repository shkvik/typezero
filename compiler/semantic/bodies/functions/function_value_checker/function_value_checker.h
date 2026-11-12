#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ResultTypes;
class NestedFunctions;

class FunctionValueChecker {
public:
  using ResultAsyncCallbackOk = Result<const Expr *, FunctionValueCheckerAsyncCallbackError>;

  FunctionValueChecker(SemanticState &owner, SemanticParts &all);

  ResultAsyncCallbackOk checkAsyncCallback(const Expr *arg, const Signature &expected,
                                           llvm::StringRef method) const;
  FunctionExpr *builtinFunctionValue(Expr *arg, const Signature &expected, unsigned primary,
                                     llvm::StringRef method);
  Expr *adaptFunction(Expr *expr, const Type *to);

private:
  using ResultArgumentsMatch = Result<size_t, FunctionValueCheckerExtraArgumentsError>;

  struct FunctionValueShape {
    llvm::StringRef name;
    unsigned maxArgs;
    bool isReturningNothing;
  };

  bool isParamFitting(const Type *param, const Type *wanted) const;
  std::optional<FunctionValueShape> functionValueShape(const Expr *arg) const;
  std::optional<FunctionValueShape> globalFunctionShape(const NameExpr *named) const;
  std::optional<FunctionValueShape> staticFunctionShape(const MemberExpr *member) const;
  std::optional<GlobalName> namedGlobal(const Expr *expr) const;
  FunctionExpr *forwardingFunction(Expr *arg, const FunctionValueShape &shape, size_t count);
  Stmt *forwardingStatement(const Expr *arg, CallExpr *call, const FunctionValueShape &shape);
  ResultArgumentsMatch checkExtraArguments(const Expr *arg, llvm::StringRef name,
                                           llvm::StringRef method, size_t count,
                                           unsigned primary) const;
  std::string qualifiedName(const Expr *arg, llvm::StringRef name) const;

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ResultTypes *resultTypes = nullptr;
  NestedFunctions *nestedFunctions = nullptr;
};

}
