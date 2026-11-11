#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class VarChecker;
class ExprChecker;
class JumpChecker;
class LoopChecker;
class ClassChecker;
class TruthChecker;
class ResultChecker;
class ReturnChecker;
class SwitchChecker;
class ClosureChecker;
class GenericChecker;

class StmtChecker {
public:
  using ResultBodyIsStatement = Result<Stmt *, StmtCheckerCheckBodyError>;

  StmtChecker(SemanticState &owner, SemanticParts &all);

  void checkIf(IfStmt *stmt);
  void checkStmt(Stmt *stmt);
  ResultBodyIsStatement checkBody(Stmt *body);

private:
  using ResultFunctionInBlock = Result<FunctionDecl *, StmtCheckerCheckFunctionStmtError>;

  ResultFunctionInBlock checkFunctionStmt(FunctionDecl *fn) const;
  void walkBlock(BlockStmt *block);
  void walkClassStmt(ClassDecl *cls);
  void walkFunctionStmt(FunctionDecl *fn);
  void walkExprStmt(ExprStmt *s);
  void walkJump(Stmt *stmt);

  SemanticState &state;
  Scopes *scopes = nullptr;
  VarChecker *varChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JumpChecker *jumpChecker = nullptr;
  LoopChecker *loopChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  ReturnChecker *returnChecker = nullptr;
  SwitchChecker *switchChecker = nullptr;
  ClosureChecker *closureChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
};

}
