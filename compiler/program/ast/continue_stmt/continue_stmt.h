#pragma once

#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct ContinueStmt : Stmt {
  ContinueStmt(llvm::SMLoc loc, llvm::StringRef label) : Stmt(Kind::Continue, loc), label(label) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Continue; }

  llvm::StringRef label; // empty without one
};

} // namespace tz
