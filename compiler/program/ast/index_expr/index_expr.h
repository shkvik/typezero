#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct EnumDecl;

class IndexExpr : public Expr {
public:
  IndexExpr(llvm::SMLoc loc, Expr *object, Expr *index)
      : Expr(Kind::Index, loc), object(object), index(index) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Index; }

  Expr *object;
  Expr *index;
  bool optional = false;
  bool mayBeOutOfRange = false;
  bool mayAppend = false;
  EnumDecl *enumDecl = nullptr;
  llvm::StringRef field;
  bool keyed = false;
};

}
