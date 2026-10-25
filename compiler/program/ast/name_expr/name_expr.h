#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

struct ClassDecl;

class NameExpr : public Expr {
public:
  NameExpr(llvm::SMLoc loc, llvm::StringRef name) : Expr(Kind::Name, loc), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Name; }

  llvm::StringRef name;
  // `first<int>`: the type arguments of a generic function, as a value or
  // as the callee of a call.
  std::vector<TypeRef> typeArgs;
  VarSymbol *var = nullptr; // set by Sema
  // Set by Sema for a top-level function used as a value.
  FuncSymbol *func = nullptr;
  // Set by Sema for a class used as a value (docs/decorators.md): the
  // object of the class.
  ClassDecl *cls = nullptr;
};

} // namespace tz
