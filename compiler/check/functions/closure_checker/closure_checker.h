#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Arrows and function expressions, and the variables they capture.
class ClosureChecker {
public:
  explicit ClosureChecker(Sema &sema);

  static Sema::Signature signatureOf(const Type *fnType);
  // `self`: the constant the function is assigned to (`const f = () => ...`),
  // through which it calls itself.
  Expr *checkFunctionExpr(FunctionExpr *fe, const Sema::Signature *expected);
  void checkClosureBody(FunctionDecl *fn);
  // `function f() {}` as an expression: f is visible in its body, in a scope
  // of its own.
  void declareOwnName(FunctionDecl *fn);
  // The statements of a block, with the functions it declares.
  void checkStatements(BlockStmt *block);

private:
  Sema &sema_;
};

} // namespace tz
