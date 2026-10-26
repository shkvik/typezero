#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ClosureChecker {
public:
  explicit ClosureChecker(Checker &checker);

  static Checker::Signature signatureOf(const Type *fnType);
  Expr *checkFunctionExpr(FunctionExpr *fe, const Checker::Signature *expected);
  void checkClosureBody(FunctionDecl *fn);
  void declareOwnName(FunctionDecl *fn);
  void checkStatements(BlockStmt *block);

private:
  Checker &checker_;
};

}
