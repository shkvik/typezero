#pragma once

#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct NestedGroup;

struct BlockStmt : Stmt {
  explicit BlockStmt(llvm::SMLoc loc) : Stmt(Kind::Block, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Block; }

  std::vector<Stmt *> body;
  NestedGroup *group = nullptr;
};

}
