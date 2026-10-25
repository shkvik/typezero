#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The return types of functions without an annotation, from their bodies.
class ReturnInferrer {
public:
  explicit ReturnInferrer(Checker &checker);

  // Return types inferred from function bodies (sema_infer.cpp). A function
  // without a return type annotation returns what its `return` statements
  // give; Checker finds that the first time it needs it.
  const Type *returnType(FuncSymbol *fn);
  // The type of the function as a value.
  const Type *functionType(FuncSymbol *fn);
  void inferReturnType(FuncSymbol *fn);
  void resolveParams(FuncSymbol *fn);
  // Checks the body of a function without a return type and sets the type
  // from its returns.
  void checkAndInfer(FunctionDecl *decl);
  // Functions that call each other, directly or not, depend on each other's
  // types: their bodies are checked on copies until the types settle.
  void inferCycle(llvm::ArrayRef<FuncSymbol *> cycle);
  // The type of the values `returns` give, with undefined if the function
  // may also end without a value; null if none has a type (yet).
  const Type *unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                           bool fallsOff, bool report);
  FunctionDecl *freshCopy(const FunctionDecl *fn);
  static bool returnsValue(const Stmt *stmt);
  // Saves the state and starts a fresh one for checking the body of `fn`:
  // only the globals of its file are visible to a top-level function, and to
  // a closure what is visible where it is declared.
  Checker::BodyState enterBody(const FunctionDecl *fn);
  void leaveBody(Checker::BodyState &saved);

private:
  Checker &checker_;
};

} // namespace tz
