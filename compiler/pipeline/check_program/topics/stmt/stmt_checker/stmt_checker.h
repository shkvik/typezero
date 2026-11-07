#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stmt/stmt_checker/stmt_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class StmtChecker {
public:
  using ResultBodyIsStatement = Result<Stmt *, StmtCheckerCheckBodyError>;

  explicit StmtChecker(CheckState &owner);

  void checkIf(IfStmt *stmt);
  void checkStmt(Stmt *stmt);
  ResultBodyIsStatement checkBody(Stmt *body);

private:
  using ResultFunctionInBlock = Result<FunctionDecl *, StmtCheckerCheckFunctionStmtError>;

  ResultFunctionInBlock checkFunctionStmt(FunctionDecl *fn) const;

  CheckState &state;
};

}
