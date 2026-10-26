#pragma once

#include "compiler/program/ast/call_expr/call_expr.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

struct ClassDecl;

class NewExpr : public Expr {
public:
  NewExpr(llvm::SMLoc loc, llvm::StringRef name, std::vector<TypeRef> typeArgs,
          std::vector<Expr *> args)
      : Expr(Kind::New, loc), name(name), typeArgs(std::move(typeArgs)), args(std::move(args)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::New; }

  llvm::StringRef name;
  llvm::StringRef member;
  std::vector<TypeRef> typeArgs;
  std::vector<Expr *> args;
  ClassDecl *cls = nullptr;
  CallExpr *ctor = nullptr;
  Expr *callee = nullptr;
  bool reflect = false;
};

}
