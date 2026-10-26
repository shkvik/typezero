#pragma once

#include "compiler/program/ast/block_stmt/block_stmt.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/var_declarator/var_declarator.h"

namespace tz {

struct TryStmt : Stmt {
  explicit TryStmt(llvm::SMLoc loc) : Stmt(Kind::Try, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Try; }

  BlockStmt *block = nullptr;
  BlockStmt *handler = nullptr;
  BlockStmt *finalizer = nullptr;
  VarDeclarator param;
};

}
