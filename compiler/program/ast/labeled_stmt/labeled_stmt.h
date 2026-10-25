#pragma once

#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

// `label: body`, a target of `break label` and, for a loop, `continue label`.
struct LabeledStmt : Stmt {
  LabeledStmt(llvm::SMLoc loc, llvm::StringRef label, Stmt *body)
      : Stmt(Kind::Labeled, loc), label(label), body(body) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Labeled; }

  llvm::StringRef label;
  Stmt *body;
};

} // namespace tz
