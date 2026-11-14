#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/blocks/code_blocks_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeBlocksRegistry {
public:
  explicit CodeBlocksRegistry(SemanticState &owner);

  void leaveBranch();
  void markUnreachable();
  void walkStmt(const WalkStmt &task);
  void walkBody(const WalkBody &task);
  void afterExprStmt(const AfterExprStmt &task);
  void finishExprStmt(const FinishExprStmt &task);

private:
  using ResultBodyIsStatement = Result<const Stmt *, CodeBlocksRegistryCheckBodyError>;
  using ResultFunctionInBlock = Result<FunctionDecl *, CodeBlocksRegistryCheckFunctionStmtError>;

  void openBranch();
  void startBlock(BlockStmt *block);
  void startExprStmt(ExprStmt *statement);
  void walkFunctionStmt(FunctionDecl *functionDecl);


  ResultBodyIsStatement checkBodyStatement(const Stmt *body) const;
  ResultFunctionInBlock checkFunctionStmt(FunctionDecl *functionDecl) const;

  SemanticState &state;
};

}
