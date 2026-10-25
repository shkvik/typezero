#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

class Type;

// A field of a class: `x: int = 0`, `static count = 0`, `readonly #id: string`,
// or a parameter property.
struct ClassField {
  llvm::StringRef name; // `#x` keeps its `#`
  llvm::SMLoc loc;
  std::optional<TypeRef> type;
  Expr *init = nullptr;
  bool optional = false; // `x?: T`
  bool definite = false; // `x!: T`, which Checker rejects
  bool readonly = false;
  bool isStatic = false;
  bool parameter = false;  // declared by a constructor parameter
  bool isOverride = false; // `override x = 1`, a field of the class extended
  Access access = Access::Public;
  // Set by Checker: the type, and for a static field its variable, a global.
  const Type *resolved = nullptr;
  VarSymbol *var = nullptr;
  // Set by Checker: a field that a class up the chain declares already; this
  // declaration only gives it another initial value (see sema_inherit.cpp).
  bool redeclared = false;
  // An arrow that nothing assigns to the field again: compiled as a method of
  // that name (see Checker::resolveClass).
  FunctionDecl *method = nullptr;
  std::vector<Decorator> decorators;
  // `accessor x = 1`, an ordinary field: reading and writing it runs no code.
  bool accessor = false;
  // `@__tz_hidden x`, in a built-in module of tz (see Module::builtin): a
  // field that is no key, as a property node computes in a getter is not
  // one, so that the object prints as node prints it.
  bool hidden = false;
};

} // namespace tz
