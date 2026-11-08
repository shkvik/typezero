#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class StmtChecker {
public:
  using ResultBodyIsStatement = Result<Stmt *, StmtCheckerCheckBodyError>;

  explicit StmtChecker(SemanticState &owner);

  void checkIf(IfStmt *stmt);
  void checkStmt(Stmt *stmt);
  ResultBodyIsStatement checkBody(Stmt *body);

private:
  using ResultFunctionInBlock = Result<FunctionDecl *, StmtCheckerCheckFunctionStmtError>;

  ResultFunctionInBlock checkFunctionStmt(FunctionDecl *fn) const;

  SemanticState &state;
};

}
