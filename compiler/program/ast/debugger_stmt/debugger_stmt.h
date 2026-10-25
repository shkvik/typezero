#pragma once

#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

// `debugger`: stops the program if a debugger is attached, and does nothing
// otherwise.
struct DebuggerStmt : Stmt {
  explicit DebuggerStmt(llvm::SMLoc loc) : Stmt(Kind::Debugger, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Debugger; }
};

} // namespace tz
