#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Which code a statement becomes; blocks, let, if and return.
class StmtLowerer {
public:
  explicit StmtLowerer(CodeGen &codegen);

  // Statements
  void emitStmt(Stmt *stmt);
  // The statements of a block, creating the functions it declares before the
  // first one that mentions them.
  void emitStatements(BlockStmt *block);
  void emitVarDecl(VarDeclStmt *decl);
  void emitIf(IfStmt *stmt);
  void emitReturn(ReturnStmt *stmt);
  // `return value`, or a plain `return` for null.
  void emitReturnValue(Expr *value);
  // Evaluates an expression for its side effects only.
  void emitDiscarded(Expr *expr);
  // Stores an owned value into a slot that may hold an older value.
  void storeReplacing(llvm::Value *address, llvm::Value *value, const Type *type);
  // The call whose result `value`, a number, is, with only arithmetic of
  // numbers after it: nothing that may throw or release; null if none.
  static const CallExpr *returnedCall(const Expr *value);

private:
  CodeGen &codegen_;
};

} // namespace tz
