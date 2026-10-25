#pragma once

#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/var_declarator/var_declarator.h"

namespace tz {

// `let a = 1, b = 2` or `const c: double = 3`.
struct VarDeclStmt : Stmt {
  VarDeclStmt(llvm::SMLoc loc, bool isConst) : Stmt(Kind::VarDecl, loc), isConst(isConst) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::VarDecl; }

  bool isConst;
  std::vector<VarDeclarator> decls;
};

} // namespace tz
