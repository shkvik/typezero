#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
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

  void walkIf(IfStmt *stmt);
  void walkStmt(Stmt *stmt);
  ResultBodyIsStatement checkBody(Stmt *body);

private:
  using ResultFunctionInBlock = Result<FunctionDecl *, StmtCheckerCheckFunctionStmtError>;

  ResultFunctionInBlock checkFunctionStmt(FunctionDecl *functionDecl) const;
  void walkBlock(BlockStmt *block);
  void walkClassStmt(ClassDecl *classDecl);
  void walkFunctionStmt(FunctionDecl *functionDecl);
  void walkExprStmt(ExprStmt *statement);
  void walkJump(Stmt *stmt);
  void walkConstantIf(IfStmt *stmt, bool constant, const Flow &whenTrue, const Flow &whenFalse);
  void walkBody(Stmt *body);

  SemanticState &state;
  Scopes *scopes = nullptr;
  VarChecker *varChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JumpChecker *jumpChecker = nullptr;
  LoopChecker *loopChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  ReturnChecker *returnChecker = nullptr;
  SwitchChecker *switchChecker = nullptr;
  ClosureChecker *closureChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
};

}
