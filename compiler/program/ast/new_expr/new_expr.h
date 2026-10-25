#pragma once

#include "compiler/program/ast/call_expr/call_expr.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

struct ClassDecl;

// `new Array<T>(n)`, `new Array<T>()`, or `new C(args)` of a class.
class NewExpr : public Expr {
public:
  NewExpr(llvm::SMLoc loc, llvm::StringRef name, std::vector<TypeRef> typeArgs,
          std::vector<Expr *> args)
      : Expr(Kind::New, loc), name(name), typeArgs(std::move(typeArgs)), args(std::move(args)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::New; }

  llvm::StringRef name;
  // `new Intl.DateTimeFormat()`: "DateTimeFormat", with "Intl" in name.
  llvm::StringRef member;
  std::vector<TypeRef> typeArgs;
  std::vector<Expr *> args;
  // Set by Checker for a class: the class, and the call of the constructor
  // variant chosen (its `func`, null when the class has no constructor),
  // which holds the checked arguments.
  ClassDecl *cls = nullptr;
  CallExpr *ctor = nullptr;
  // `new route.cls()`, `new (Mixin(User))()`: a class as a value
  // (docs/decorators.md, sections 5 and 6), evaluated first. The parser sets
  // it for anything but a name, Checker for a name of a variable too. Of a
  // class known when compiling it is evaluated and dropped, and `cls` is
  // made; of a `Constructor<T>` (`reflect` set) the class makes the object
  // through its description, from `args` as unknowns.
  Expr *callee = nullptr;
  bool reflect = false;
};

} // namespace tz
