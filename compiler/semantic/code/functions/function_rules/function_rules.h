#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>

namespace tz {

class FunctionRules {
public:
  explicit FunctionRules(SemanticState &owner);

  FunctionExpr *builtinFunctionValue(Expr *arg, const Signature &expected, unsigned primary,
                                     llvm::StringRef method);

private:
  struct FunctionValueShape {
    llvm::StringRef name;
    unsigned maxArgs;
    bool isReturningNothing;
  };

  using ResultArgumentsMatch =
      Result<size_t, CodeFunctionsRegistryFunctionValueExtraArgumentsError>;

  std::optional<FunctionValueShape> functionValueShape(const Expr *arg) const;
  std::optional<FunctionValueShape> globalFunctionShape(const NameExpr *named) const;
  std::optional<FunctionValueShape> staticFunctionShape(const MemberExpr *member) const;


  std::optional<GlobalName> namedGlobal(const Expr *expr) const;
  std::string qualifiedName(const Expr *arg, llvm::StringRef name) const;
  FunctionExpr *forwardingFunction(Expr *arg, const FunctionValueShape &shape, size_t count);
  Stmt *forwardingStatement(const Expr *arg, CallExpr *call, const FunctionValueShape &shape);
  ResultArgumentsMatch checkExtraValueArguments(const Expr *arg, llvm::StringRef name,
                                                llvm::StringRef method, size_t count,
                                                unsigned primary) const;

  SemanticState &state;
};

}
