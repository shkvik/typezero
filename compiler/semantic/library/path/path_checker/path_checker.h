#pragma once

#include "compiler/program/library/path/path_function_table/path_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/path/path_checker/path_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class TypeRelations;
class ArgumentChecker;

class PathChecker {
public:
  PathChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef functionName,
                      llvm::SMLoc loc);
  Expr *checkPathValue(NodeModule module, llvm::StringRef name, Expr *expr);

private:
  using ResultPathProperty = Result<llvm::StringRef, PathCheckerPathValueError>;
  using ResultPathFunction = Result<llvm::StringRef, PathCheckerPathFunctionError>;

  ResultPathFunction pathFunction(llvm::StringRef functionName) const;
  ResultPathProperty pathValue(llvm::StringRef name, NodeModule module) const;
  const Type *parsedPathType() const;
  const Type *formatInputType() const;
  Expr *checkArgsLoosely(CallExpr *call);
  Expr *checkJoinedPath(CallExpr *call, Expr *style, Builtin builtin, llvm::SMLoc loc);
  Expr *checkFormatPath(CallExpr *call, Expr *style);
  bool fitsFormatInput(const Type *type) const;
  Expr *checkTextPath(CallExpr *call, Expr *style, const PathFunctionInfo &path);
  const Type *pathResultType(PathFunctionForm form) const;

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
