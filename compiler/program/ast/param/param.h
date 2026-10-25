#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

class Type;

struct Param {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> type;
  bool optional = false;        // `name?: T`
  bool rest = false;            // `...name: T[]`, the last parameter
  Expr *defaultValue = nullptr; // `name: T = value`
  // A parameter property, `constructor(public x: int)`: a field of the class
  // too, which the constructor sets first (the parser adds `this.x = x`).
  bool property = false;
  bool readonly = false;
  Access access = Access::Public;
  // A parameter written as a destructuring pattern, `{ x, y }: Point`, has a
  // hidden name, and the body starts by destructuring it; this is the
  // pattern as written, for messages.
  llvm::StringRef pattern;
  std::vector<Decorator> decorators;
  llvm::StringRef label() const { return pattern.empty() ? name : pattern; }
  // Set by Checker: the type of the parameter inside the function, and its
  // variable.
  const Type *declaredType = nullptr;
  VarSymbol *sym = nullptr;
};

} // namespace tz
