#pragma once

#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/var_declarator/var_declarator.h"

namespace tz {

struct VarDeclStmt : Stmt {
  VarDeclStmt(llvm::SMLoc loc, bool isConst) : Stmt(Kind::VarDecl, loc), isConst(isConst) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::VarDecl; }

  bool isConst;
  std::vector<VarDeclarator> decls;
};

}
