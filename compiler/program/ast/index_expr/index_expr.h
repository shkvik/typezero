#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct EnumDecl;

// `object[index]`
class IndexExpr : public Expr {
public:
  IndexExpr(llvm::SMLoc loc, Expr *object, Expr *index)
      : Expr(Kind::Index, loc), object(object), index(index) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Index; }

  Expr *object;
  Expr *index;
  bool optional = false; // `object?.[index]`
  // Set by Sema. A read that Sema could not prove in range checks the index
  // and yields `T | undefined`. A write may append: Sema proved only
  // `index <= length`.
  bool mayBeOutOfRange = false;
  bool mayAppend = false;
  // Set by Sema for `Color[value]`: the name of an enum member.
  EnumDecl *enumDecl = nullptr;
  // Set by Sema for `o[key]` of an object or a tuple with a constant key: the
  // field it reads, which Sema reads by name instead.
  llvm::StringRef field;
  // Set by Sema for `o[k]` of an object whose key is of a union of string
  // literals (`Record<"up" | "down", int>`), each naming a field of one
  // type: the field is chosen when the program runs.
  bool keyed = false;
};

} // namespace tz
