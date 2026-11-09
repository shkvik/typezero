#pragma once

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

  Expr *checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkPathValue(NodeModule module, llvm::StringRef name, Expr *expr);

private:
  using ResultPathProperty = Result<llvm::StringRef, PathCheckerPathValueError>;
  using ResultPathFunction = Result<llvm::StringRef, PathCheckerPathFunctionError>;

  static ResultPathFunction pathFunction(llvm::StringRef fn);
  static ResultPathProperty pathValue(llvm::StringRef name, NodeModule module);
  static const Type *parsedPathType();
  static const Type *formatInputType();

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
