#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

struct ClassDecl;

class NameExpr : public Expr {
public:
  NameExpr(llvm::SMLoc loc, llvm::StringRef name) : Expr(Kind::Name, loc), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Name; }

  llvm::StringRef name;
  std::vector<TypeRef> typeArgs;
  VarSymbol *var = nullptr;
  FuncSymbol *func = nullptr;
  ClassDecl *cls = nullptr;
};

}
