#pragma once

#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct DebuggerStmt : Stmt {
  explicit DebuggerStmt(llvm::SMLoc loc) : Stmt(Kind::Debugger, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Debugger; }
};

}
