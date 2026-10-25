#pragma once

#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct BreakStmt : Stmt {
  BreakStmt(llvm::SMLoc loc, llvm::StringRef label) : Stmt(Kind::Break, loc), label(label) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Break; }

  llvm::StringRef label; // empty without one
};

} // namespace tz
