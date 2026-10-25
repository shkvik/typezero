#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

// A conversion inserted by Sema: a numeric widening, ToInt32 for an operand
// of a bitwise operator, or turning a value into a string for `+` and
// template literals. `target` is set only for `x as T` in the source, which
// Sema rejects (see Sema::checkCast).
class CastExpr : public Expr {
public:
  CastExpr(llvm::SMLoc loc, Expr *operand, std::optional<TypeRef> target)
      : Expr(Kind::Cast, loc), operand(operand), target(target) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Cast; }

  bool isImplicit() const { return !target; }

  Expr *operand;
  std::optional<TypeRef> target;
  // A link of an optional chain, `operand?.x`: from `T | undefined` to T,
  // where undefined ends the whole chain (see OptionalChainExpr).
  bool shortCircuit = false;
  // To a string, the text of a value of a union (`${x}`, String(x)), which
  // may hold another member than the string: not a narrowing.
  bool text = false;
  // A function value given a type whose result its own converts to, `(r:
  // Request) => Promise<Response>` where `(r: Request) => Response |
  // Promise<Response>` is expected: a function that calls it and converts
  // what it gives (see Sema::adaptResult).
  bool adaptsResult = false;
};

} // namespace tz
