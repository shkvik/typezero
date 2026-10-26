#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class StmtLowerer {
public:
  explicit StmtLowerer(Lowering &lowering);

  void emitStmt(Stmt *stmt);
  void emitStatements(BlockStmt *block);
  void emitVarDecl(VarDeclStmt *decl);
  void emitIf(IfStmt *stmt);
  void emitReturn(ReturnStmt *stmt);
  void emitReturnValue(Expr *value);
  void emitDiscarded(Expr *expr);
  void storeReplacing(llvm::Value *address, llvm::Value *value, const Type *type);
  static const CallExpr *returnedCall(const Expr *value);

private:
  Lowering &lowering_;
};

}
