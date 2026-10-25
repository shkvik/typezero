#pragma once

#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct NestedGroup;

struct BlockStmt : Stmt {
  explicit BlockStmt(llvm::SMLoc loc) : Stmt(Kind::Block, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Block; }

  std::vector<Stmt *> body;
  // Set by Sema: the functions declared in this block, if any.
  NestedGroup *group = nullptr;
};

} // namespace tz
