#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"

namespace tz {

class MemberExpr : public Expr {
public:
  MemberExpr(llvm::SMLoc loc, Expr *object, llvm::StringRef name)
      : Expr(Kind::Member, loc), object(object), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Member; }

  Expr *object;
  llvm::StringRef name;
  bool optional = false; // `object?.name`
  // Set by Checker for a method read as a value: a function bound to the object.
  // With `virtualCall`, the method of the object's class at the slot of
  // `method`, as for CallExpr.
  FuncSymbol *method = nullptr;
  // `JSON.parse<User>(text)`: the type arguments of the callee of a call.
  std::vector<TypeRef> typeArgs;
  bool virtualCall = false;
  // A method of an interface with several signatures, as a value: which one.
  int signature = -1;
};

} // namespace tz
