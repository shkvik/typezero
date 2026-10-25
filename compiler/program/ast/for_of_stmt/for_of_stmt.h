#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/var_declarator/var_declarator.h"

namespace tz {

// `for (const x of iterable) body`: over the elements of an array, the code
// points of a string, or the values of an iterator. With `keys`,
// `for (const k in object) body`: Sema makes the iterable the array of the
// object's keys.
struct ForOfStmt : Stmt {
  explicit ForOfStmt(llvm::SMLoc loc) : Stmt(Kind::ForOf, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::ForOf; }

  bool isConst = false;
  bool keys = false;
  // `for await (const x of iterable)`: over the values of an async iterator,
  // or the elements of an array or an iterator, each awaited.
  bool isAwait = false;
  VarDeclarator binding; // no initializer
  Expr *iterable = nullptr;
  Stmt *body = nullptr;
  // Set by Sema: the loop variable may borrow each element from the array,
  // without reference counting.
  bool borrowElements = false;
};

} // namespace tz
