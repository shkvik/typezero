#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class StmtChecker {
public:
  using ResultBodyIsStatement = Result<Stmt *, StmtCheckerCheckBodyError>;

  explicit StmtChecker(Checker &owner);

  void checkIf(IfStmt *stmt);
  void checkStmt(Stmt *stmt);
  ResultBodyIsStatement checkBody(Stmt *body);

private:
  using ResultFunctionInBlock = Result<FunctionDecl *, StmtCheckerCheckFunctionStmtError>;

  ResultFunctionInBlock checkFunctionStmt(FunctionDecl *fn) const;

  Checker &checker;
};

}
